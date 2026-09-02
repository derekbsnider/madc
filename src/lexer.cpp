//////////////////////////////////////////////////////////////////////////
//									//
// madc lexer methods to tokenize a source file into tokens		//
//									//
//////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <deque>
#include <queue>
#include <stack>
#include <functional>
#include <chrono>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_dl.h"
#include "madc_posix_io.h"	// resolve_real_path (canonical_path_for_compare's primitive)
#include "madc_pch.h"
#include "madc_sys_includes.h"	// generated per-stdlib-flavor include search tables
#include "madc_mangle.h"	// std ABI namespace push (note_std_abi_define)
#include "spelling_delim.h"
#include "libc_signatures.h"	// THE C99 math root list (shared with the parser)

// Out-of-line anchor for intern_keyed_map's env-gated write trap
// (MADC_MAPWRITE_TRAP=<key>): break on madcdis_mapwrite_trap_hit in gdb to
// catch every string-keyed insertion of a poisoned key, whoever the caller.
namespace madc { namespace dis {
void madcdis_mapwrite_trap_hit(const char *key)
{
    fprintf(stderr, "[maptrap] write key=%s\n", key);
}
} }
#include "cir_freeze.h"	// Phase 6: CirFrozenForest — parse-time grove binding
#include "rt/rt_task.h"	// MT-3b: the token pumps honor task cancellation

// Stage-2 cooperative parse: yield-point cadence for the token pumps (a
// power of two — the pump check is one mask-and-compare). ~1k tokens is
// well under a millisecond, far finer than the ~10ms slice budget the
// stage-2 ruling set; parse_yield_point itself no-ops without live tasks.
enum { LEX_YIELD_GRAIN = 1024 };

// MT-3b: a cancelled task's tokenize aborts CLEANLY at its next yield
// point — a C++ throw the pump's own catch(const char *) handler records
// as a lexer diagnostic (tokenize returns NULL). Never the SJLJ cancel
// throw here: it would longjmp past the lexer's C++ frames.
static void lex_abort_if_task_cancelled(void)
{
	if (__madc_task_cancelled())
		throw "tokenize cancelled (task cancellation)";
}

// --show-stats: RAII accumulator for time spent loading source into the lex
// stream (file read / embedded-header copy). Adds its lifetime, in seconds, to
// the referenced accumulator on scope exit.
namespace {
struct ReadTimer
{
    double &acc;
    std::chrono::steady_clock::time_point t0;
    ReadTimer(double &a) : acc(a), t0(std::chrono::steady_clock::now()) {}
    ~ReadTimer()
    {
	acc += std::chrono::duration<double>(
	    std::chrono::steady_clock::now() - t0).count();
    }
};
}

// From precompiled_headers.cpp (generated)
struct PrecompiledHeader { const uint8_t *data; size_t size; };
extern const PrecompiledHeader *find_precompiled_header(const std::string &name);
static bool find_filesystem_precompiled_header(Program &pgm,
					       const std::string &incfile,
					       bool is_system,
					       std::string &outpath);

using namespace std;

// The ONE replacement-text interpreter (check-one-macro-expander.sh): every
// scan over macro/fragment spelling delegates here — defined with the macro
// expansion machinery below.
static std::vector<Program::MacroDef::ReplacementToken> tokenize_macro_spelling(const std::string &text);

// R4-full shared prelude cache. A cached image is immutable lexer output plus
// the exact post-preprocessor state produced by ONE pure embedded fragment.
// TokenBase itself is deliberately absent: parser-side fields on those shells
// are mutable, so every sibling TU materializes a fresh shell from TokenRec.
struct MadcSharedPreludeCache
{
    struct TokenImage
    {
	TokenRec rec;
	TokenType type = TokenType::ttBase;
	std::string spelling;
	std::string source_text;
	long double real_value = 0;
	DataDef *datatype = NULL;
	int trivia_count = 0;
	bool wide = false;
    };

    struct Entry
    {
	struct MacroDelta
	{
	    enum Kind { undefine, define_object, define_function } kind;
	    std::string name;
	    std::string value;
	    Program::MacroDef macro;
	};
	std::vector<TokenImage> tokens;
	std::vector<MacroDelta> macro_deltas;
	std::string header;
	std::string include_key;
    };

    struct MacroState
    {
	enum Kind { absent, object, function } kind = absent;
	std::string name;
	std::string value;
	Program::MacroDef macro;
    };

    std::map<std::string, Entry> entries;

    // The embedded text is process-constant and every Program in the group
    // shares one spelling universe. Discover lexical dependencies and possible
    // macro writes once, then probe each TU's flat maps by spelling id.
    std::map<std::string, std::vector<uint32_t> > dependency_ids;
    std::map<std::string, std::vector<std::string> > mutation_names;

    static void append_u64(std::string &out, uint64_t value)
    {
	out.append(reinterpret_cast<const char *>(&value), sizeof(value));
    }

    static void append_blob(std::string &out, const std::string &value)
    {
	append_u64(out, (uint64_t)value.size());
	out.append(value.data(), value.size());
    }

    static bool embedded_fragment_is_pure(
	const std::string &text, std::vector<std::string> *mutations = NULL)
    {
	// Only conditional/object-macro directives have state fully represented
	// by define_map/macro_map below. Includes, loads, pragmas, and line/error
	// controls keep using the literal include owner.
	if ( text.find("_Pragma") != std::string::npos )
	    return false;
	std::set<std::string> seen_mutations;
	if ( mutations )
	    mutations->clear();
	for ( size_t pos = 0; pos < text.size(); )
	{
	    size_t end = text.find('\n', pos);
	    if ( end == std::string::npos )
		end = text.size();
	    size_t p = pos;
	    while ( p < end && (text[p] == ' ' || text[p] == '\t') )
		++p;
	    if ( p < end && text[p] == '#' )
	    {
		++p;
		while ( p < end && (text[p] == ' ' || text[p] == '\t') )
		    ++p;
		size_t word_at = p;
		while ( p < end && (isalpha((unsigned char)text[p]) || text[p] == '_') )
		    ++p;
		std::string directive = text.substr(word_at, p - word_at);
		if ( directive != "define" && directive != "undef"
		  && directive != "if" && directive != "ifdef"
		  && directive != "ifndef" && directive != "elif"
		  && directive != "else" && directive != "endif" )
		    return false;
		if ( directive == "define" || directive == "undef" )
		{
		    while ( p < end && (text[p] == ' ' || text[p] == '\t') )
			++p;
		    size_t name_at = p;
		    if ( p == end
		      || !(isalpha((unsigned char)text[p]) || text[p] == '_') )
			return false;
		    ++p;
		    while ( p < end
			 && (isalnum((unsigned char)text[p]) || text[p] == '_') )
			++p;
		    std::string name = text.substr(name_at, p - name_at);
		    // A single textual mutation per name makes final-state replay
		    // equivalent to directive-event replay, including inactive arms.
		    if ( !seen_mutations.insert(name).second )
			return false;
		    if ( mutations )
			mutations->push_back(name);
		}
	    }
	    pos = end < text.size() ? end + 1 : end;
	}
	return true;
    }

    static bool candidate(Program &pgm, const std::string &header)
    {
	if ( !pgm.compile_group || pgm.pack_recording || pgm.keep_trivia
	  || pgm.suppress_auto_include_scan || !pgm._pending_pack_ops.empty()
	  || !pgm.ifdef_stack.empty() || !pgm.ifdef_done_stack.empty()
	  || !pgm._macro_save_stack.empty() )
	    return false;
	const std::string *embedded = find_embedded_header(header);
	if ( !embedded || !pgm.is_embedded_header_allowed(header)
	  || pgm.embedded_header_outranked(header)
	  || find_precompiled_header(header) )
	    return false;
	std::string pch_path;
	if ( find_filesystem_precompiled_header(pgm, header, true, pch_path) )
	    return false;
	return embedded_fragment_is_pure(*embedded);
    }

    // Replacement/fragment spelling is interpreted by the ONE owner
    // (tokenize_macro_spelling — the check-one-macro-expander gate): collect
    // identifier spellings, and report whether the text carries a ## paste
    // (paste can synthesize a macro name that is not lexically visible, so
    // such state keys on the full preprocessor map). The tokenizer also gets
    // raw strings and pp-numbers right, which the hand scan this replaces did
    // not (an identifier-shaped tail of `123abc` is not a macro reference).
    static bool collect_identifiers(const std::string &text,
				    std::set<std::string> &names)
    {
	typedef Program::MacroDef::ReplacementToken T;
	const std::vector<T> tokens = tokenize_macro_spelling(text);
	bool has_paste = false;
	for ( size_t i = 0; i < tokens.size(); ++i )
	{
	    if ( tokens[i].kind == T::rtIdentifier )
		names.insert(text.substr(tokens[i].begin,
					 tokens[i].end - tokens[i].begin));
	    else if ( tokens[i].kind == T::rtPaste )
		has_paste = true;
	}
	return has_paste;
    }

    static bool macro_equal(const Program::MacroDef &a,
			    const Program::MacroDef &b)
    {
	return a.params == b.params && a.variadic == b.variadic
	    && a.variadic_param == b.variadic_param && a.body == b.body;
    }

    static void append_all_macro_state(Program &pgm, std::string &key)
    {
	append_u64(key, (uint64_t)pgm.define_map.size());
	pgm.define_map.for_each([&key](const char *name, std::string &value) {
	    append_blob(key, name ? std::string(name) : std::string());
	    append_blob(key, value);
	    return false;
	});
	append_u64(key, (uint64_t)pgm.macro_map.size());
	pgm.macro_map.for_each([&key](const char *name, Program::MacroDef &macro) {
	    append_blob(key, name ? std::string(name) : std::string());
	    append_u64(key, (uint64_t)macro.params.size());
	    for ( size_t i = 0; i < macro.params.size(); ++i )
		append_blob(key, macro.params[i]);
	    append_u64(key, macro.variadic ? 1 : 0);
	    append_blob(key, macro.variadic_param);
	    append_blob(key, macro.body);
	    return false;
	});
    }

    const std::vector<uint32_t> &dependencies_for(Program &pgm,
						   const std::string &header)
    {
	std::map<std::string, std::vector<uint32_t> >::iterator cached =
	    dependency_ids.find(header);
	if ( cached != dependency_ids.end() )
	    return cached->second;
	std::set<std::string> names;
	const std::string *text = find_embedded_header(header);
	if ( text )
	    collect_identifiers(*text, names);
	std::vector<uint32_t> ids;
	ids.reserve(names.size());
	for ( std::set<std::string>::const_iterator ni = names.begin();
	      ni != names.end(); ++ni )
	    ids.push_back(pgm.strpool.intern(*ni));
	return dependency_ids.insert(std::make_pair(header, ids)).first->second;
    }

    const std::vector<std::string> &mutations_for(const std::string &header)
    {
	std::map<std::string, std::vector<std::string> >::iterator cached =
	    mutation_names.find(header);
	if ( cached != mutation_names.end() )
	    return cached->second;
	std::vector<std::string> names;
	const std::string *text = find_embedded_header(header);
	if ( text )
	    embedded_fragment_is_pure(*text, &names);
	return mutation_names.insert(std::make_pair(header, names)).first->second;
    }

    std::string make_key(Program &pgm, const std::string &header)
    {
	std::string key;
	append_blob(key, header);
	append_u64(key, (uint64_t)pgm.language_std);
	append_u64(key, pgm.gnu_dialect ? 1 : 0);
	const std::vector<uint32_t> &base = dependencies_for(pgm, header);
	std::set<uint32_t> dependencies(base.begin(), base.end());
	std::vector<uint32_t> work(base.begin(), base.end());
	bool needs_full_state = false;
	for ( size_t wi = 0; wi < work.size(); ++wi )
	{
	    uint32_t name = work[wi];
	    const std::string *object = pgm.define_map.find_readonly(name);
	    const Program::MacroDef *function = pgm.macro_map.find_readonly(name);
	    if ( object )
	    {
		append_u64(key, 1);
		append_blob(key, *object);
		std::set<std::string> nested;
		if ( collect_identifiers(*object, nested) )
		    needs_full_state = true;
		for ( std::set<std::string>::const_iterator ni = nested.begin();
		      ni != nested.end(); ++ni )
		{
		    uint32_t nested_id = pgm.strpool.intern(*ni);
		    if ( dependencies.insert(nested_id).second )
			work.push_back(nested_id);
		}
	    }
	    else if ( function )
	    {
		append_u64(key, 2);
		append_u64(key, (uint64_t)function->params.size());
		for ( size_t i = 0; i < function->params.size(); ++i )
		    append_blob(key, function->params[i]);
		append_u64(key, function->variadic ? 1 : 0);
		append_blob(key, function->variadic_param);
		append_blob(key, function->body);
		std::set<std::string> nested;
		if ( collect_identifiers(function->body, nested) )
		    needs_full_state = true;
		for ( std::set<std::string>::const_iterator ni = nested.begin();
		      ni != nested.end(); ++ni )
		{
		    uint32_t nested_id = pgm.strpool.intern(*ni);
		    if ( dependencies.insert(nested_id).second )
			work.push_back(nested_id);
		}
	    }
	    else
		append_u64(key, 0);
	}
	// Token-paste can synthesize a macro name that is not lexically visible.
	// Keep such fragments exact by falling back to the full preprocessor key.
	append_u64(key, needs_full_state ? 1 : 0);
	if ( needs_full_state )
	    append_all_macro_state(pgm, key);
	std::string include_key = "<" + header + ">";
	std::map<std::string, bool>::const_iterator ii =
	    pgm.included_files.find(include_key);
	append_u64(key, ii != pgm.included_files.end() && ii->second ? 1 : 0);
	return key;
    }

    static MacroState macro_state(Program &pgm, const std::string &name)
    {
	MacroState state;
	state.name = name;
	const std::string *object = pgm.define_map.find_readonly(name);
	const Program::MacroDef *function = pgm.macro_map.find_readonly(name);
	if ( object )
	{
	    state.kind = MacroState::object;
	    state.value = *object;
	}
	else if ( function )
	{
	    state.kind = MacroState::function;
	    state.macro = *function;
	}
	return state;
    }

    static void capture_macro_deltas(Program &pgm,
				      const std::vector<MacroState> &before,
				      Entry &entry)
    {
	for ( size_t i = 0; i < before.size(); ++i )
	{
	    const MacroState &old = before[i];
	    MacroState now = macro_state(pgm, old.name);
	    if ( old.kind == now.kind
	      && (now.kind != MacroState::object || old.value == now.value)
	      && (now.kind != MacroState::function
		  || macro_equal(old.macro, now.macro)) )
		continue;
	    Entry::MacroDelta delta;
	    delta.name = now.name;
	    if ( now.kind == MacroState::object )
	    {
		delta.kind = Entry::MacroDelta::define_object;
		delta.value = now.value;
	    }
	    else if ( now.kind == MacroState::function )
	    {
		delta.kind = Entry::MacroDelta::define_function;
		delta.macro = now.macro;
	    }
	    else
		delta.kind = Entry::MacroDelta::undefine;
	    entry.macro_deltas.push_back(delta);
	}
    }

    static bool snapshot_token(Program &pgm, TokenBase *tb, TokenImage &image)
    {
	if ( !tb || !tb->leading_trivia.empty()
	  || pgm._pragma_pack_events.count(tb) )
	    return false;
	image.rec = tb->rec;
	image.rec.slot_id = 0;
	image.type = tb->type();
	image.datatype = tb->datadef();
	if ( image.type == TokenType::ttIdentifier
	  || image.type == TokenType::ttString
	  || image.type == TokenType::ttComment
	  || image.type == TokenType::ttKeyword
	  || image.type == TokenType::ttDataType )
	{
	    TokenIdent *ident = (TokenIdent *)tb;
	    image.spelling.assign(ident->spelling(), ident->spelling_len());
	}

	switch ( image.type )
	{
	case TokenType::ttInteger:
	    image.source_text = ((TokenInt *)tb)->source_text;
	    return true;
	case TokenType::ttReal:
	    image.real_value = ((TokenReal *)tb)->ldval();
	    image.source_text = ((TokenReal *)tb)->source_text;
	    return true;
	case TokenType::ttString:
	    image.wide = ((TokenStr *)tb)->wide;
	    return true;
	case TokenType::ttSpace:
	    image.trivia_count = ((TokenSpace *)tb)->cnt;
	    return true;
	case TokenType::ttTab:
	    image.trivia_count = ((TokenTab *)tb)->cnt;
	    return true;
	case TokenType::ttEOL:
	    image.trivia_count = ((TokenEOL *)tb)->cnt;
	    return true;
	case TokenType::ttComment:
	case TokenType::ttIdentifier:
	case TokenType::ttChar:
	case TokenType::ttDataType:
	case TokenType::ttKeyword:
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	case TokenType::ttSymbol:
	    return true;
	default:
	    if ( getenv("MADC_PRELUDE_CACHE_PROBE") )
		fprintf(stderr, "[prelude-cache] unsupported token type=%d id=%d\n",
			(int)image.type, (int)tb->id());
	    return false;
	}
    }

    static bool entry_compatible(Program &pgm, const Entry &entry)
    {
	for ( size_t i = 0; i < entry.tokens.size(); ++i )
	{
	    const TokenImage &image = entry.tokens[i];
	    if ( image.type == TokenType::ttDataType
	      && pgm.datatype_map.find(image.spelling) == pgm.datatype_map.end() )
		return false;
	}
	return true;
    }

    static TokenBase *materialize_token(Program &pgm, const TokenImage &image,
					uint32_t &last_file_id,
					const char *&last_file)
    {
	TokenBase *tb = NULL;
	TokenID kind = (TokenID)image.rec.kind;
	switch ( image.type )
	{
	case TokenType::ttIdentifier:
	    tb = pgm.make_ident(image.spelling);
	    break;
	case TokenType::ttInteger:
	    tb = image.source_text.empty()
	       ? pgm.make_int(image.rec.value)
	       : pgm.make_int(image.rec.value, image.source_text);
	    break;
	case TokenType::ttReal:
	    tb = pgm.make_real(image.real_value);
	    ((TokenReal *)tb)->source_text = image.source_text;
	    break;
	case TokenType::ttString:
	    tb = pgm.make_str(image.spelling, image.wide);
	    break;
	case TokenType::ttChar:
	    tb = pgm.make_char((int)image.rec.value);
	    break;
	case TokenType::ttDataType:
	{
	    flat_datatype_map_iter di = pgm.datatype_map.find(image.spelling);
	    if ( di == pgm.datatype_map.end() )
		return NULL;
	    tb = pgm.make_datatype(image.spelling.c_str(), (*di)->definition);
	    break;
	}
	case TokenType::ttComment:
	    tb = pgm.make_rem(image.spelling);
	    break;
	case TokenType::ttSpace:
	    tb = pgm.make_space(image.trivia_count);
	    break;
	case TokenType::ttTab:
	    tb = pgm.make_tab(image.trivia_count);
	    break;
	case TokenType::ttEOL:
	    tb = pgm.make_eol(image.trivia_count);
	    break;
	case TokenType::ttKeyword:
	    if ( kind == TokenID::tkCPPKEYWORD )
		tb = new TokenCppKeyword(image.spelling);
	    else
		tb = pgm.make_token(kind);
	    break;
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	case TokenType::ttSymbol:
	    tb = pgm.make_token(kind);
	    break;
	default:
	    return NULL;
	}
	if ( !tb )
	    return NULL;
	if ( image.datatype )
	    tb->setDataType(image.datatype);
	uint32_t slot_id = tb->rec.slot_id;
	tb->rec = image.rec;
	tb->rec.slot_id = slot_id;
	if ( image.rec.file_id != last_file_id )
	{
	    last_file_id = image.rec.file_id;
	    last_file = last_file_id
		? pgm.intern_file(pgm.strpool.str(last_file_id)) : NULL;
	}
	tb->file = last_file;
	tb->line = image.rec.line;
	tb->column = image.rec.column;
	return tb;
    }

    static bool restore(Program &pgm, const Entry &entry)
    {
	if ( !entry_compatible(pgm, entry) )
	    return false;
	uint32_t last_file_id = 0;
	const char *last_file = NULL;
	for ( size_t i = 0; i < entry.tokens.size(); ++i )
	{
	    TokenBase *tb = materialize_token(pgm, entry.tokens[i],
				       last_file_id, last_file);
	    if ( !tb )
		return false;
	    ++pgm._tok_produced;
	    pgm.tokens.push_back(tb);
	}
	for ( size_t i = 0; i < entry.macro_deltas.size(); ++i )
	{
	    const Entry::MacroDelta &delta = entry.macro_deltas[i];
	    pgm.define_map.erase(delta.name);
	    pgm.macro_map.erase(delta.name);
	    if ( delta.kind == Entry::MacroDelta::define_object )
	    {
		pgm.define_map[delta.name] = delta.value;
		pgm.note_std_abi_define(delta.name, delta.value);
	    }
	    else if ( delta.kind == Entry::MacroDelta::define_function )
		pgm.macro_map[delta.name] = delta.macro;
	}
	pgm.included_files[entry.include_key] = true;
	// The restored tokens ENTERED this TU's live stream — record it
	// through the one recording owner (live_tokenize_record), exactly
	// like the miss path's should_tokenize_include named-key arm, or
	// the v40 prune's forest_unit_file_live_tokenized evidence misses
	// cache-hit headers.
	pgm.live_tokenize_record(entry.include_key, true);
	pgm.mark_embedded_include_flag(entry.header);
	return true;
    }

    static bool capture(
	Program &pgm, const std::string &header, size_t begin,
	const std::vector<MacroState> &before_macros,
	Entry &entry)
    {
	if ( !pgm._pending_pack_ops.empty() || !pgm._macro_save_stack.empty()
	  || !pgm.ifdef_stack.empty() || !pgm.ifdef_done_stack.empty() )
	    return false;
	entry.tokens.reserve(pgm.tokens.size() - begin);
	for ( size_t i = begin; i < pgm.tokens.size(); ++i )
	{
	    TokenImage image;
	    if ( !snapshot_token(pgm, pgm.tokens[i], image) )
		return false;
	    entry.tokens.push_back(image);
	}
	capture_macro_deltas(pgm, before_macros, entry);
	entry.header = header;
	entry.include_key = "<" + header + ">";
	return true;
    }
};

static bool find_filesystem_precompiled_header(Program &pgm,
					       const std::string &incfile,
					       bool is_system,
					       std::string &outpath);
static bool load_precompiled_header_file(const std::string &path,
					 std::deque<TokenBase *> &tokens);
static bool push_precompiled_header_tokens(Program &pgm,
					   const std::string &display_name,
					   std::deque<TokenBase *> &pch_tokens);
static bool named_include_provider_exists(Program &pgm,
					  const std::string &incfile,
					  std::string &pch_path);

static DataDef *get_complex_compat_type(DataDef *base_type)
{
    return Program::complex_type_of(base_type);
}

static bool is_binary_prefix(int ch, Source &source)
{
    return ch == '0' && source.good() && (source.peek() == 'b' || source.peek() == 'B');
}

static bool is_digit_separator(int ch)
{
    return ch == '\'';
}

static uint32_t read_utf8_codepoint(Source &source, unsigned char first)
{
    if ( first < 0x80 )
	return first;
    int need = 0;
    uint32_t cp = first;
    if ( (first & 0xE0) == 0xC0 )
    {
	need = 1;
	cp = first & 0x1F;
    }
    else if ( (first & 0xF0) == 0xE0 )
    {
	need = 2;
	cp = first & 0x0F;
    }
    else if ( (first & 0xF8) == 0xF0 )
    {
	need = 3;
	cp = first & 0x07;
    }
    else
	return first;

    while ( need-- > 0 )
    {
	if ( !source.good() )
	    return first;
	unsigned char next = (unsigned char)source.peek();
	if ( (next & 0xC0) != 0x80 )
	    return first;
	source.get();
	cp = (cp << 6) | (next & 0x3F);
    }
    return cp;
}

static uint32_t read_literal_escape_value(Source &source, char esc)
{
    switch ( esc )
    {
	case 'n':  return '\n';
	case 't':  return '\t';
	case 'r':  return '\r';
	case '\\': return '\\';
	case '"':  return '"';
	case '\'': return '\'';
	case 'a':  return '\a';
	case 'b':  return '\b';
	case 'f':  return '\f';
	case 'v':  return '\v';
	case '?':  return '\?';
	case 'x': case 'X': {
	    uint32_t val = 0;
	    int dig = 0;
	    while ( dig < 2 && source.good() )
	    {
		int c = source.peek();
		int d = (c >= '0' && c <= '9') ? c - '0'
		    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
		    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if ( d < 0 )
		    break;
		val = (val << 4) | (uint32_t)d;
		source.get();
		++dig;
	    }
	    return val;
	}
	case 'u': case 'U': {
	    // Universal-character-name ([lex.charset]): \uXXXX / \UXXXXXXXX.
	    // Only the wide/prefixed-literal reader routes escapes here, so
	    // narrow-string escape handling is untouched.
	    int need = esc == 'u' ? 4 : 8;
	    uint32_t val = 0;
	    int dig = 0;
	    while ( dig < need && source.good() )
	    {
		int c = source.peek();
		int d = (c >= '0' && c <= '9') ? c - '0'
		    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
		    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if ( d < 0 )
		    break;
		val = (val << 4) | (uint32_t)d;
		source.get();
		++dig;
	    }
	    return val;
	}
	case '0': case '1': case '2': case '3':
	case '4': case '5': case '6': case '7': {
	    uint32_t val = (uint32_t)(esc - '0');
	    int dig = 1;
	    while ( dig < 3 && source.good() )
	    {
		int c = source.peek();
		if ( c < '0' || c > '7' )
		    break;
		val = (val << 3) | (uint32_t)(c - '0');
		source.get();
		++dig;
	    }
	    return val;
	}
	default:
	    return (unsigned char)esc;
    }
}

static void append_wide_codepoint(std::string &out, uint32_t cp)
{
    out += (char)(cp & 0xff);
    out += (char)((cp >> 8) & 0xff);
    out += (char)((cp >> 16) & 0xff);
    out += (char)((cp >> 24) & 0xff);
}

static void append_narrow_string_as_wide(std::string &out,
					 const std::string &narrow)
{
    for ( unsigned char c : narrow )
	append_wide_codepoint(out, (uint32_t)c);
}

// Encode one codepoint as UTF-8 bytes (u8"..." literal storage — madc narrow
// strings are UTF-8 byte strings, so a u8 string IS a narrow string).
static void append_utf8_codepoint(std::string &out, uint32_t cp)
{
    if ( cp < 0x80 )
	out += (char)cp;
    else if ( cp < 0x800 )
    {
	out += (char)(0xC0 | (cp >> 6));
	out += (char)(0x80 | (cp & 0x3F));
    }
    else if ( cp < 0x10000 )
    {
	out += (char)(0xE0 | (cp >> 12));
	out += (char)(0x80 | ((cp >> 6) & 0x3F));
	out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
	out += (char)(0xF0 | (cp >> 18));
	out += (char)(0x80 | ((cp >> 12) & 0x3F));
	out += (char)(0x80 | ((cp >> 6) & 0x3F));
	out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string narrow_string_as_wide(const std::string &narrow)
{
    std::string out;
    append_narrow_string_as_wide(out, narrow);
    return out;
}

TokenBase *Program::read_wide_literal(const std::string &prefix)
{
    char quote = source.get();
    int row = source.line();
    int col = source.column();
    if ( quote == '"' )
    {
	// u"..." (UTF-16 units) has no faithful storage in the 4-byte-unit
	// wide model or the narrow byte model — loud, not silently wrong.
	if ( prefix == "u" )
	    throw "UTF-16 string literals (u\"...\") are not supported";
	// On the LLP64 target the ONE wide model is wchar_t = 2-byte UTF-16
	// (addWideLiteral re-encodes the UTF-32 payload), so U"..." (char32_t
	// units) loses its element width there — same loud rule as u"...".
	if ( prefix == "U" && target_llp64() )
	    throw "char32_t string literals (U\"...\") are not supported on the LLP64 target";
	std::string bytes;
	while ( source.good() && source.peek() != '"' )
	{
	    uint32_t cp;
	    if ( source.peek() == '\\' )
	    {
		source.get();
		if ( !source.good() )
		    break;
		cp = read_literal_escape_value(source, source.get());
	    }
	    else
		cp = read_utf8_codepoint(source, (unsigned char)source.get());
	    // u8"..." stores UTF-8 bytes (a narrow madc string IS UTF-8);
	    // L"..." / U"..." store 4-byte UTF-32 units in the TOKEN payload
	    // on every target (LP64: wchar_t and char32_t share the 32-bit
	    // layout; LLP64: addWideLiteral re-encodes the payload to the
	    // 2-byte UTF-16 wchar_t shape at Variable mint, and U"..." was
	    // rejected above).
	    if ( prefix == "u8" )
		append_utf8_codepoint(bytes, cp);
	    else
		append_wide_codepoint(bytes, cp);
	}
	if ( !source.good() )
	{
	    source.setpos(row, col);
	    throw "Unterminated wide string";
	}
	source.get();
	{
	    TokenBase *stok = make_str(bytes, prefix != "u8");
	    // Piece extent includes the encoding prefix (L"..."/u8"...").
	    if ( source.line() == row )
	    {
		TokenStr::SrcPiece pc;
		pc.line = row;
		pc.col = col - 1 - (int32_t)prefix.size();
		pc.len = source.column() - pc.col;
		((TokenStr *)stok)->src_pieces.push_back(pc);
	    }
	    return stok;
	}
    }

    uint32_t cp = 0;
    while ( source.good() && source.peek() != '\'' )
    {
	if ( source.peek() == '\\' )
	{
	    source.get();
	    if ( !source.good() )
		break;
	    cp = read_literal_escape_value(source, source.get());
	}
	else
	    cp = read_utf8_codepoint(source, (unsigned char)source.get());
    }
    if ( !source.good() )
    {
	source.setpos(row, col);
	throw "Unterminated wide character literal";
    }
    source.get();
    TokenInt *ti = (TokenInt *)make_int((int64_t)cp);
    // [lex.ccon] literal types: L'' -> wchar_t (target-shaped:
    // dd_platform_wchar()), U'' -> char32_t (uint32), u'' -> char16_t
    // (uint16), u8'' -> char8_t (uint8).
    ti->setDataType(prefix == "U" ? static_cast<DataDef *>(&ddUINT32)
		  : prefix == "u" ? static_cast<DataDef *>(&ddUINT16)
		  : prefix == "u8" ? static_cast<DataDef *>(&ddUINT8)
		  : dd_platform_wchar());
    return ti;
}

static unsigned __int128 read_binary_literal(Source &source, bool &ovf)
{
    unsigned __int128 bv = 0;
    ovf = false;
    source.get(); // eat 'b' / 'B'
    while ( source.good() )
    {
	if ( is_digit_separator(source.peek()) )
	{
	    source.get();
	    continue;
	}
	if ( source.peek() != '0' && source.peek() != '1' )
	    break;
	if ( (bv >> 127) != 0 )
	    ovf = true;		// shifted past 128 bits
	bv <<= 1;
	bv += source.get() - '0';
    }
    return bv;
}

static bool consume_macro_call_open(Source &source)
{
    std::string spacing;

    // C preprocessor: function-like macro calls allow whitespace
    // INCLUDING newlines between the macro name and '('.
    while ( source.good()
	 && (source.peek() == ' ' || source.peek() == '\t'
	  || source.peek() == '\n' || source.peek() == '\r') )
	spacing += source.get();
    if ( source.good() && source.peek() == '(' )
    {
	source.get(); // consume '('
	return true;
    }
    if ( !spacing.empty() )
	source.pushback(spacing);
    return false;
}

// Raw preprocessing spellings have not reached the token stream yet.  Shield
// quotes and comments here, then let SpellingDelimDepth remain the sole owner
// of delimiter bookkeeping.  Callers keep their own policy (argument commas,
// copied text, query operands) on top of this state.
class PpLexicalShield
{
    enum class Mode {
	Code, String, Character, LineComment, BlockComment, BlockCommentClose
    };

    Mode mode = Mode::Code;
    bool escaped = false;

public:
    bool structural(char c, char next)
    {
	if ( mode == Mode::LineComment )
	{
	    if ( c == '\n' || c == '\r' )
		mode = Mode::Code;
	    return false;
	}
	if ( mode == Mode::BlockComment )
	{
	    if ( c == '*' && next == '/' )
		mode = Mode::BlockCommentClose;
	    return false;
	}
	if ( mode == Mode::BlockCommentClose )
	{
	    // This byte is the `/` already paired with the preceding `*`.
	    // Resume code only AFTER it, so `*//` is a closed comment followed
	    // by one slash rather than the start of a line comment.
	    mode = Mode::Code;
	    return false;
	}
	if ( mode == Mode::String || mode == Mode::Character )
	{
	    if ( escaped )
	    {
		escaped = false;
		return false;
	    }
	    if ( c == '\\' )
	    {
		escaped = true;
		return false;
	    }
	    if ( (mode == Mode::String && c == '"')
	      || (mode == Mode::Character && c == '\'') )
		mode = Mode::Code;
	    return false;
	}
	if ( c == '"' )
	{
	    mode = Mode::String;
	    return false;
	}
	if ( c == '\'' )
	{
	    mode = Mode::Character;
	    return false;
	}
	if ( c == '/' && next == '/' )
	{
	    mode = Mode::LineComment;
	    return false;
	}
	if ( c == '/' && next == '*' )
	{
	    mode = Mode::BlockComment;
	    return false;
	}
	return true;
    }
};

class PpGroupScan
{
    SpellingDelimDepth depth;
    PpLexicalShield shield;
    bool started = false;
    bool finished = false;

public:
    bool step(char c, char next)
    {
	if ( !shield.structural(c, next) )
	    return false;
	int before = depth.paren;
	depth.update(c);
	if ( !started && c == '(' && depth.paren == 1 )
	    started = true;
	if ( started && c == ')' && before == 1 && depth.paren == 0 )
	    finished = true;
	return true;
    }

    bool at_argument_level() const { return started && depth.paren == 1; }
    bool closed() const { return finished; }
};

// Consume one parenthesized preprocessing group, including its delimiters.
static bool consume_pp_group(Source &source, std::string *copy = NULL)
{
    if ( !source.good() || source.peek() != '(' )
	return false;
    PpGroupScan group;
    while ( source.good() )
    {
	char c = source.get();
	if ( copy )
	    *copy += c;
	group.step(c, source.good() ? source.peek() : '\0');
	if ( group.closed() )
	    return true;
    }
    return false;
}

// Return the first byte after a balanced preprocessing group in text.
static size_t pp_group_end(const std::string &text, size_t open)
{
    if ( open >= text.size() || text[open] != '(' )
	return std::string::npos;
    PpGroupScan group;
    for ( size_t i = open; i < text.size(); ++i )
    {
	char next = i + 1 < text.size() ? text[i + 1] : '\0';
	group.step(text[i], next);
	if ( group.closed() )
	    return i + 1;
    }
    return std::string::npos;
}

static bool is_identifier_spelling(const std::string &s)
{
    if ( s.empty() )
	return false;
    if ( s[0] != '_' && !isalpha((unsigned char)s[0]) )
	return false;
    for ( size_t i = 1; i < s.size(); ++i )
	if ( s[i] != '_' && !isalnum((unsigned char)s[i]) )
	    return false;
    return true;
}

static bool identifier_matches_gnu_attribute_name(const std::string &id,
						  const std::string &name)
{
    return id == name || id == "__" + name + "__";
}

GnuAttributeKind madc_gnu_attribute_kind(const std::string &name)
{
    struct Entry {
	const char *name;
	GnuAttributeKind kind;
    };
    static const Entry entries[] = {
	{ "packed", GnuAttributeKind::Packed },
	{ "aligned", GnuAttributeKind::Aligned },
	{ "mode", GnuAttributeKind::Mode },
	{ "scalar_storage_order", GnuAttributeKind::ScalarStorageOrder },
	{ "vector_size", GnuAttributeKind::VectorSize },
	{ "alias", GnuAttributeKind::Alias },
	{ "no_instrument_function", GnuAttributeKind::NoInstrumentFunction },
	{ "optimize", GnuAttributeKind::Optimize },
	{ "using_if_exists", GnuAttributeKind::UsingIfExists }
    };
    for ( size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i )
	if ( identifier_matches_gnu_attribute_name(name, entries[i].name) )
	    return entries[i].kind;
    return GnuAttributeKind::Unsupported;
}

static bool gnu_attribute_text_has_supported_name(const std::string &text)
{
    for ( size_t i = 0; i < text.size(); )
    {
	char c = text[i];
	if ( c == '"' || c == '\'' )
	{
	    char quote = c;
	    ++i;
	    while ( i < text.size() )
	    {
		if ( text[i] == '\\' && i + 1 < text.size() )
		{
		    i += 2;
		    continue;
		}
		if ( text[i++] == quote )
		    break;
	    }
	    continue;
	}
	if ( c != '_' && !isalpha((unsigned char)c) )
	{
	    ++i;
	    continue;
	}
	std::string id;
	while ( i < text.size()
	     && (text[i] == '_' || isalnum((unsigned char)text[i])) )
	    id += text[i++];
	if ( madc_gnu_attribute_kind(id) != GnuAttributeKind::Unsupported )
	    return true;
    }
    return false;
}

static int compound_type_specifier_flag(const std::string &w)
{
    enum {
	TS_CHAR     = 1 << 2,
	TS_SHORT    = 1 << 4,
	TS_INT      = 1 << 6,
	TS_LONG     = 1 << 8,
	TS_FLOAT    = 1 << 10,
	TS_DOUBLE   = 1 << 12,
	TS_SIGNED   = 1 << 14,
	TS_UNSIGNED = 1 << 16,
	TS_COMPLEX  = 1 << 18,
	TS_INT128   = 1 << 20,
	// C23 _FloatN family, combinable with _Complex only (gcc's
	// avx512fp16intrin.h: `_Float16 _Complex __A`). Two flags, one per
	// approximation class (float / double) — the exact SPELLING for the
	// minted token comes from the words themselves, not the bit.
	TS_FLOATN_F = 1 << 22,	// _Float16, _Float32  (~float)
	TS_FLOATN_D = 1 << 24,	// _Float64/_Float128/_Float32x/_Float64x (~double)
    };
    if ( w == "char" ) return TS_CHAR;
    if ( w == "short" ) return TS_SHORT;
    if ( w == "int" ) return TS_INT;
    if ( w == "long" ) return TS_LONG;
    if ( w == "float" ) return TS_FLOAT;
    if ( w == "double" ) return TS_DOUBLE;
    if ( w == "signed" ) return TS_SIGNED;
    if ( w == "unsigned" ) return TS_UNSIGNED;
    if ( w == "__int128" ) return TS_INT128;
    if ( w == "_Complex" || w == "__complex__" || w == "__complex" ) return TS_COMPLEX;
    if ( w == "_Float16" || w == "_Float32" ) return TS_FLOATN_F;
    if ( w == "_Float64" || w == "_Float128"
      || w == "_Float32x" || w == "_Float64x" ) return TS_FLOATN_D;
    return 0;
}

static bool expansion_is_compound_type_specifiers(const std::string &text, int &flags)
{
    flags = 0;
    size_t i = 0;
    bool saw_any = false;
    while ( i < text.size() )
    {
	while ( i < text.size() && isspace((unsigned char)text[i]) )
	    ++i;
	if ( i >= text.size() )
	    break;
	if ( text[i] != '_' && !isalpha((unsigned char)text[i]) )
	    return false;
	std::string word;
	while ( i < text.size() && (text[i] == '_' || isalnum((unsigned char)text[i])) )
	    word += text[i++];
	int flag = compound_type_specifier_flag(word);
	if ( !flag )
	    return false;
	flags += flag;
	saw_any = true;
    }
    return saw_any;
}

static const char *auto_include_header_for_identifier(const std::string &word)
{
    static const std::map<std::string, std::string> identifier_headers = {
	{"string", "string"},
	{"stringstream", "sstream"},

	{"cout", "iostream"},
	{"cin", "iostream"},
	{"cerr", "iostream"},
	{"clog", "iostream"},
	{"endl", "iostream"},

	{"ifstream", "fstream"},
	{"ofstream", "fstream"},
	{"fstream", "fstream"},

	{"getline", "string"},
	{"to_string", "string"},
	{"stoi", "string"},
	{"stol", "string"},
	{"stod", "string"},

	{"vector", "vector"},
	{"map", "map"},
	{"set", "set"},

	// std::format / std::print / std::println — compiler-implemented
	// intrinsics; the fragment declares them (madc dialect only, like
	// every entry here — auto_includes_enabled gates the scan).
	{"format", "bits/std_format"},
	{"print", "bits/std_format"},
	{"println", "bits/std_format"},

	// The stdio streams: std::print(stderr, ...) in a zero-include
	// script needs stderr's lazy registration, which rides the
	// stdio.h include flag.
	{"stderr", "stdio.h"},
	{"stdout", "stdio.h"},
	{"stdin", "stdio.h"},

	{"php", "ns_php"},
	{"perl", "ns_perl"},
	{"python", "ns_python"},
	{"ruby", "ns_ruby"},
	{"js", "ns_js"},
	{"rust", "ns_rust"},
	{"madc", "ns_madc"},
	{"ui", "ns_ui"},

	{"size_t", "stddef.h"},
	{"ptrdiff_t", "stddef.h"},
	{"wchar_t", "stddef.h"},
	{"NULL", "stddef.h"},
	{"offsetof", "stddef.h"},

	{"intptr_t", "stdint.h"},
	{"uintptr_t", "stdint.h"},
	{"INT8_MIN", "stdint.h"},
	{"INT8_MAX", "stdint.h"},
	{"UINT8_MAX", "stdint.h"},
	{"INT16_MIN", "stdint.h"},
	{"INT16_MAX", "stdint.h"},
	{"UINT16_MAX", "stdint.h"},
	{"INT32_MIN", "stdint.h"},
	{"INT32_MAX", "stdint.h"},
	{"UINT32_MAX", "stdint.h"},
	{"INT64_MIN", "stdint.h"},
	{"INT64_MAX", "stdint.h"},
	{"UINT64_MAX", "stdint.h"},
	{"SIZE_MAX", "stdint.h"},
	{"INTMAX_MIN", "stdint.h"},
	{"INTMAX_MAX", "stdint.h"},
	{"UINTMAX_MAX", "stdint.h"},
	{"PTRDIFF_MIN", "stdint.h"},
	{"PTRDIFF_MAX", "stdint.h"},

	{"FLT_RADIX", "float.h"},
	{"FLT_EVAL_METHOD", "float.h"},
	{"DECIMAL_DIG", "float.h"},
	{"FLT_MANT_DIG", "float.h"},
	{"DBL_MANT_DIG", "float.h"},
	{"LDBL_MANT_DIG", "float.h"},
	{"FLT_DIG", "float.h"},
	{"DBL_DIG", "float.h"},
	{"LDBL_DIG", "float.h"},
	{"FLT_MIN_EXP", "float.h"},
	{"DBL_MIN_EXP", "float.h"},
	{"LDBL_MIN_EXP", "float.h"},
	{"FLT_MIN_10_EXP", "float.h"},
	{"DBL_MIN_10_EXP", "float.h"},
	{"LDBL_MIN_10_EXP", "float.h"},
	{"FLT_MAX_EXP", "float.h"},
	{"DBL_MAX_EXP", "float.h"},
	{"LDBL_MAX_EXP", "float.h"},
	{"FLT_MAX_10_EXP", "float.h"},
	{"DBL_MAX_10_EXP", "float.h"},
	{"LDBL_MAX_10_EXP", "float.h"},
	{"FLT_MAX", "float.h"},
	{"DBL_MAX", "float.h"},
	{"LDBL_MAX", "float.h"},
	{"FLT_MIN", "float.h"},
	{"DBL_MIN", "float.h"},
	{"LDBL_MIN", "float.h"},
	{"FLT_EPSILON", "float.h"},
	{"DBL_EPSILON", "float.h"},
	{"LDBL_EPSILON", "float.h"}
    };

    std::map<std::string, std::string>::const_iterator it = identifier_headers.find(word);
    if ( it == identifier_headers.end() )
	return NULL;
    return it->second.c_str();
}

static std::vector<std::string> ordered_auto_include_headers(const std::set<std::string> &headers)
{
    static const char *preferred_order[] = {
	"stddef.h",
	"stdint.h",
	"float.h",
	"stdlib.h",
	"string.h",
	"stdio.h",
	"string",
	"sstream",
	"iostream",
	"fstream",
	"vector",
	"map",
	"set",
	"ns_php",
	"ns_perl",
	"ns_python",
	"ns_ruby",
	"ns_js",
	"ns_rust",
	"ns_madc",
	"ns_ui",
	NULL
    };

    std::vector<std::string> ordered;
    std::set<std::string> emitted;
    for ( int i = 0; preferred_order[i]; ++i )
    {
	std::string h(preferred_order[i]);
	if ( headers.count(h) )
	{
	    ordered.push_back(h);
	    emitted.insert(h);
	}
    }
    for ( std::set<std::string>::const_iterator it = headers.begin();
	  it != headers.end(); ++it )
	if ( !emitted.count(*it) )
	    ordered.push_back(*it);
    return ordered;
}

static size_t auto_include_insertion_index(const TokenStream &tokens,
					   size_t limit,
					   const char *source_name,
					   const std::set<const char *> &user_units)
{
    if ( !source_name || !*source_name )
	return 0;
    for ( size_t i = 0; i < limit; ++i )
    {
	TokenBase *tb = tokens[i];
	if ( !tb || !tb->file )
	    continue;
	// The prelude belongs BEFORE the first USER-CODE token: the main
	// file, or any QUOTED user module (auto_include_user_units — the
	// same system-vs-user discriminator the identifier scan applies).
	// Matching only the main file put the fragment AFTER a module's
	// tokens, so a bare `format` inside `#include "mod.inc"` parsed
	// before its declaration existed.
	if ( strcmp(tb->file, source_name) == 0
	  || user_units.count(tb->file) )
	    return i;
    }
    return limit;
}

void Program::mark_embedded_include_flag(const std::string &incfile)
{
    if ( incfile == "iostream" )
    {
	_include_iostream = true;
	_include_string = true;
    }
    if ( incfile == "stdio.h" )
	_include_stdio = true;
    if ( incfile == "string" )
	_include_string = true;
    if ( incfile == "ns_madc" )
	_include_ns_madc = true;
}

// Trivia tokens exist in the stream only under keep_trivia; every
// backward position probe must step over them the same way.
static bool is_trivia_token(const TokenBase *t)
{
    TokenType tt = t->type();
    return tt == TokenType::ttSpace || tt == TokenType::ttTab
	|| tt == TokenType::ttEOL || tt == TokenType::ttComment;
}

bool Program::auto_include_standard_identifier(const std::string &word,
					       bool positional)
{
    if ( skip_includes )
	return false;
    if ( !auto_includes_enabled() )
	return false;
    if ( suppress_auto_include_scan )
	return false;

    // `typedef unsigned long size_t;` and similar declaration heads are
    // defining the identifier, not using the standard header surface.
    // Auto-including here injects the embedded header in the middle of
    // the declarator and leaves a duplicate alias token behind.
    for ( auto it = tokens.rbegin(); positional && it != tokens.rend(); ++it )
    {
	TokenBase *t = *it;
	TokenID tid = t->id();
	TokenType tt = t->type();
	if ( tid == TokenID::tkMul || is_trivia_token(t) )
	    continue;
	if ( tt == TokenType::ttDataType
	  || tid == TokenID::tkSTRUCT
	  || tid == TokenID::tkCLASS
	  || tid == TokenID::tkENUM
	  || tid == TokenID::tkCONST
	  || tid == TokenID::tkEXTERN
	  || tid == TokenID::tkSTATIC
	  || tid == TokenID::tkREGISTER
	  || tid == TokenID::tkTYPEDEF
	  || tid == TokenID::tkRESTRICT )
	    return false;
	// An identifier in member-access position (`G.player.set`, `p->set`)
	// or qualified by anything other than `std` (`ui::set`,
	// `madc::getline`) cannot denote the std-header surface — matching
	// it would pull that header's whole include tree into the TU.
	// `std::set` still qualifies; the qualifier itself (`ui` in
	// `ui::set`) is scanned at its own lex time, unaffected.
	if ( tid == TokenID::tkDot || tid == TokenID::tkDeRef )
	    return false;
	if ( tid == TokenID::tkNS )
	{
	    for ( ++it; it != tokens.rend(); ++it )
	    {
		TokenBase *q = *it;
		if ( is_trivia_token(q) )
		    continue;
		if ( q->type() != TokenType::ttIdentifier
		  || !((TokenIdent *)q)->spelling_is("std") )
		    return false;
		break;
	    }
	}
	break;
    }

    const char *header = auto_include_header_for_identifier(word);
    if ( !header )
	return false;

    // Host policy must not be bypassed by the auto-include convenience: the
    // literal-include path deliberately falls through to the filesystem when
    // an embedded stub is policy-disallowed (explicit includes resolve or
    // error, gcc canon), and include/madc/ can exist on disk — so a QUEUED
    // disallowed header would serve anyway. An identifier match never queues
    // one; the name stays unknown and the parse-time diagnostic ("Unknown
    // namespace or class 'php'") is the host's contract
    // (test_libmadc_program security_policy case).
    if ( find_embedded_header(header) && !is_embedded_header_allowed(header) )
	return false;
    // The namespace-head table entries additionally respect the per-namespace
    // registration policy (security_policy.allow_*_namespace) — the check
    // answers true for every non-namespace word, so the std-surface entries
    // are unaffected.
    if ( !is_namespace_registration_enabled(word) )
	return false;

    pending_auto_include_headers.insert(header);
    pending_auto_include_identifiers.insert(word);
    return false;
}

std::vector<TokenBase *> Program::tokenize_auto_include_define(const std::string &value,
							       const TokenBase *origin)
{
    std::vector<TokenBase *> replacement;
    if ( value.empty() )
	return replacement;

    Source saved = std::move(source);
    bool saved_suppress_auto_include_scan = suppress_auto_include_scan;
    suppress_auto_include_scan = true;
    source = Source();
    source.fname(origin && origin->file ? origin->file : "<auto-include>");
    source.str(value);

    try
    {
	TokenBase *rt;
	while ( (rt = getRealToken()) )
	{
	    if ( origin )
	    {
		rt->file = origin->file;
		rt->line = origin->line;
		rt->column = origin->column;
		// The replacement spelling is not the source's bytes at the
		// origin position (`NULL` -> `((void *)0)`): coordinate
		// consumers (parse_spans) must skip these (tfSYNTHPOS).
		rt->setFlag(tfSYNTHPOS);
	    }
	    replacement.push_back(rt);
	}
    }
    catch(...)
    {
	source = std::move(saved);
	suppress_auto_include_scan = saved_suppress_auto_include_scan;
	throw;
    }

    source = std::move(saved);
    suppress_auto_include_scan = saved_suppress_auto_include_scan;
    return replacement;
}

void Program::tokenize_synthetic_system_include(const std::string &header,
						 const char *origin_name)
{
	Source saved = std::move(source);
	bool saved_suppress_auto_include_scan = suppress_auto_include_scan;
	suppress_auto_include_scan = true;
	source = Source();
	source.fname(origin_name);
	std::string directive = std::string("#include <") + header + ">\n";
	{ ReadTimer _rt(_read_seconds); source.str(directive); }
	TokenBase *itb;
	while ( (itb = getRealToken()) )
		push_token_with_literal_concat(itb);
	source = std::move(saved);
	suppress_auto_include_scan = saved_suppress_auto_include_scan;
}

void Program::tokenize_embedded_header_text(const std::string &name,
					    const std::string &text,
					    bool protocol_visit)
{
	// A protocol visit forms no unit and no edge: its serving belongs to the
	// includer. A normal embedded header is a distinct forest unit reached by
	// the includer's edge.
	const char *protocol_saved = NULL;
	if ( protocol_visit )
		protocol_saved = pack_protocol_serving_begin();
	else
		pack_record_edge(name);
	Source saved = std::move(source);
	bool saved_suppress_auto_include_scan = suppress_auto_include_scan;
	suppress_auto_include_scan = true;
	source = Source();
	source.fname(name.c_str());
	{ ReadTimer _rt(_read_seconds); source.str(text); }
	_input_bytes += text.size();
	TokenBase *itb;
	const char *interned = intern_file(name);
	if ( !protocol_visit )
	{
		pack_note_unit(interned);
		pack_record_source(interned, text);
	}
	// v40 (task #57): the embedded twin of the filesystem include's
	// unit-stack tracking — embedded units freeze too.
	if ( pack_recording && !protocol_visit )
	{
		for ( size_t si = 0; si < pack_unit_stack.size(); ++si )
			pack_unit_subtree[pack_unit_stack[si]].insert(interned);
		pack_unit_subtree[interned].insert(interned);
		pack_unit_stack.push_back(interned);
	}
	while ( (itb = getRealToken()) )
	{
		itb->file = interned;
		push_token_with_literal_concat(itb);
	}
	if ( pack_recording && !protocol_visit )
		pack_unit_stack.pop_back();
	source = std::move(saved);
	suppress_auto_include_scan = saved_suppress_auto_include_scan;
	if ( protocol_visit )
		pack_protocol_serving_end(protocol_saved);
	mark_embedded_include_flag(name);
}

// The madc dialect's value stream inserter (bits/value_stream) is part of
// madc's own always-included surface: madc::value is an inherent madc-mode
// type, so streaming one must not depend on any user #include. Its
// DECLARATION still cannot precede std::ostream's, so it rides the include
// machinery's completions: the first time an include finishes with the
// ostream guard defined (a live #define, a PCH replay, or a forest bind's
// PP-export install — macro_name_defined sees all three), the fragment is
// tokenized once, landing right after that header's tokens. The PROTOCOL
// serving mode keeps it out of the pack: no unit, no edge.
//
// TOP-LEVEL completions only: the guard is defined at the TOP of <ostream>,
// long before basic_ostream / the ostream typedef are declared, so a NESTED
// include's completion inside it would inject the fragment mid-header into
// undeclared territory. suppress_auto_include_scan is true exactly while a
// header (or synthetic prelude) serving is in flight, so it doubles as the
// at-top-level test; the prelude's own servings are covered by the explicit
// call in inject_pending_auto_includes, after suppression is restored.
void Program::maybe_inject_value_stream_operator()
{
    if ( _value_stream_operator_injected )
	return;
    if ( suppress_auto_include_scan )
	return;
    if ( language_std != STD_MADC )
	return;
    if ( !macro_name_defined("_GLIBCXX_OSTREAM")
      && !macro_name_defined("_LIBCPP_OSTREAM") )
	return;
    const std::string *fragment = find_embedded_header("bits/value_stream");
    if ( !fragment )
	return;
    _value_stream_operator_injected = true;	// before serving: reentry guard
    tokenize_embedded_header_text("bits/value_stream", *fragment, true);
}

// Every include-serving arm returns through here once its serving is
// COMPLETE (tokens pushed / unit bound / PCH replayed) — the one place
// include-completion side effects fire. Skip/defer arms (once-only dedup,
// auto-include deferral) keep the plain getToken() return: they changed no
// preprocessor state. Keep new serving arms on this return.
TokenBase *Program::include_completed_token()
{
    maybe_inject_value_stream_operator();
    return getToken();
}

void Program::expand_pending_auto_include_macros(size_t original_start)
{
    if ( pending_auto_include_identifiers.empty() )
	return;

    std::vector<TokenBase *> rewritten;
    for ( size_t i = 0; i < tokens.size(); ++i )
    {
	TokenBase *tb = tokens[i];
	if ( i >= original_start
	  && tb
	  && tb->type() == TokenType::ttIdentifier )
	{
	    TokenIdent *ident = (TokenIdent *)tb;
	    if ( pending_auto_include_identifiers.count(ident->spelling()) )
	    {
		madc::dis::intern_keyed_map<std::string>::iterator di =
		    define_map.find(ident->spelling());
		if ( di != define_map.end() )
		{
		    std::vector<TokenBase *> repl =
			tokenize_auto_include_define(*di, tb);
		    rewritten.insert(rewritten.end(), repl.begin(), repl.end());
		    continue;
		}
	    }
	}
	rewritten.push_back(tb);
    }

    tokens.assign_ids_from(rewritten);
    pending_auto_include_identifiers.clear();
}

void Program::inject_pending_auto_includes()
{
    static const bool prelude_probe = getenv("MADC_PRELUDE_CACHE_PROBE") != NULL;
    if ( skip_includes )
	return;
    if ( !auto_includes_enabled() )
    {
	pending_auto_include_headers.clear();
	pending_auto_include_identifiers.clear();
	return;
    }
    if ( pending_auto_include_headers.empty() )
	return;

    size_t include_start = tokens.size();
    size_t insert_at = auto_include_insertion_index(tokens, include_start,
						    source.fname(),
						    auto_include_user_units);

    while ( !pending_auto_include_headers.empty() )
    {
	std::set<std::string> batch;
	batch.swap(pending_auto_include_headers);
	std::vector<std::string> ordered = ordered_auto_include_headers(batch);
	for ( std::vector<std::string>::const_iterator hi = ordered.begin();
	      hi != ordered.end(); ++hi )
	{
	    const std::string &header = *hi;
	    MadcSharedPreludeCache *preludes = NULL;
	    std::string prelude_key;
	    std::vector<MadcSharedPreludeCache::MacroState> prelude_before_macros;
	    if ( MadcSharedPreludeCache::candidate(*this, header) )
	    {
		if ( !compile_group->shared_preludes )
		    compile_group->shared_preludes.reset(new MadcSharedPreludeCache());
		preludes = compile_group->shared_preludes.get();
		prelude_key = preludes->make_key(*this, header);
		std::map<std::string, MadcSharedPreludeCache::Entry>::const_iterator ci =
		    preludes->entries.find(prelude_key);
		if ( ci != preludes->entries.end()
		  && MadcSharedPreludeCache::restore(*this, ci->second) )
		{
		    ++compile_group->shared_prelude_hits;
		    compile_group->shared_prelude_tokens += ci->second.tokens.size();
		    if ( prelude_probe )
			fprintf(stderr, "[prelude-cache] hit %s (%zu tokens)\n",
				header.c_str(), ci->second.tokens.size());
		    continue;
		}
		++compile_group->shared_prelude_misses;
		if ( prelude_probe )
		    fprintf(stderr, "[prelude-cache] miss %s\n", header.c_str());
		const std::vector<std::string> &mutations =
		    preludes->mutations_for(header);
		prelude_before_macros.reserve(mutations.size());
		for ( size_t mi = 0; mi < mutations.size(); ++mi )
		    prelude_before_macros.push_back(
			MadcSharedPreludeCache::macro_state(*this, mutations[mi]));
	    }

	    size_t fragment_start = tokens.size();
	    uint32_t boundary_spelling = fragment_start
		? tokens[fragment_start - 1]->rec.spelling_id : 0;
	    size_t forest_units_before = forest_chain_set.size();
	    // Delegate to the LITERAL include owner: feed a synthetic
	    // `#include <hdr>` line through the real directive handler, so
	    // forest bind, PCH discovery, embedded text, the filesystem
	    // walk, once-only dedup, pack edges and include flags all apply
	    // identically to an include the user wrote. The injector keeps
	    // NO private serving copy of that chain: its old PCH+embedded
	    // arms silently dropped every header without a named provider,
	    // which is how retiring the embedded <string>/<sstream> twins
	    // broke the C++ arm of auto-include unnoticed.
	    tokenize_synthetic_system_include(header, "<auto-include>");

	    // A direct embedded serving is pure lexer/preprocessor work. Cache its
	    // immutable output only when the literal owner confirms that no forest
	    // unit was adopted and no leading string literal merged backward across
	    // the captured range. Forest/PCH/nested-include paths stay authoritative.
	    bool boundary_unchanged = !fragment_start
		|| tokens[fragment_start - 1]->rec.spelling_id == boundary_spelling;
	    if ( preludes && forest_chain_set.size() == forest_units_before
	      && boundary_unchanged )
	    {
		MadcSharedPreludeCache::Entry entry;
		bool captured = MadcSharedPreludeCache::capture(
			*this, header, fragment_start, prelude_before_macros, entry);
		if ( captured )
		    preludes->entries[prelude_key] = entry;
		if ( prelude_probe )
		    fprintf(stderr, "[prelude-cache] %s %s (%zu tokens)\n",
			    captured ? "store" : "decline", header.c_str(),
			    entry.tokens.size());
	    }
	}
    }

    // The prelude's synthetic includes complete under suppression, so the
    // include-completion seam could not fire for them; this is their
    // top-level completion point (a bare-cout script's <iostream> arrives
    // here). Appending before the move keeps the fragment inside the
    // relocated prelude range.
    maybe_inject_value_stream_operator();

    if ( tokens.size() > include_start )
    {
	// Move the just-appended auto-include token range [include_start, end)
	// to insert_at (id-level; runs at cursor==0, pushback empty).
	size_t tail_len = tokens.size() - include_start;
	tokens.move_tail_to(include_start, insert_at);
	expand_pending_auto_include_macros(insert_at + tail_len);
    }
    else
	expand_pending_auto_include_macros(insert_at);
}

// Walk back through recently emitted tokens to decide whether the
// current identifier is at a declaration / definition head, i.e.
// `<type> [*...] <ident> (...)`. When so, we must suppress
// function-like macro expansion: otherwise `#define bug(...) ((void)0)`
// above a later `void bug(const char *, ...)` definition eats the
// declarator and the parse fails. Skips pointer decorators; stops at
// the first non-`*` token and classifies it as type / qualifier /
// typedef-identifier (→ decl head) or anything else (→ not decl head).
static bool looks_like_decl_head(const TokenStream &tokens)
{
    for ( auto it = tokens.rbegin(); it != tokens.rend(); ++it )
    {
	TokenBase *t = *it;
	TokenID tid = t->id();
	TokenType tt = t->type();
	if ( tid == TokenID::tkMul ) continue;
	if ( tt == TokenType::ttDataType ) return true;
	if ( tid == TokenID::tkSTRUCT || tid == TokenID::tkCLASS
	  || tid == TokenID::tkENUM ) return true;
	if ( tid == TokenID::tkCONST || tid == TokenID::tkEXTERN
	  || tid == TokenID::tkSTATIC || tid == TokenID::tkREGISTER
	  || tid == TokenID::tkTYPEDEF || tid == TokenID::tkRESTRICT ) return true;
	return false;
    }
    return false;
}

static bool decl_head_macro_args_look_like_prototype(const std::vector<std::string> &args)
{
    if ( args.empty() )
	return true;

    auto starts_with_type_word = [](const std::string &raw) -> bool {
	size_t i = 0;
	while ( i < raw.size() && (raw[i] == ' ' || raw[i] == '\t') )
	    ++i;
	size_t start = i;
	while ( i < raw.size() && (raw[i] == '_' || isalnum((unsigned char)raw[i])) )
	    ++i;
	std::string word = raw.substr(start, i - start);
	return word == "void" || word == "char" || word == "short"
	    || word == "int" || word == "long" || word == "float"
	    || word == "double" || word == "signed" || word == "unsigned"
	    || word == "const" || word == "volatile" || word == "restrict"
	    || word == "struct" || word == "union" || word == "enum"
	    || word == "class" || word == "register" || word == "extern"
	    || word == "static" || word == "typedef";
    };

    // EVERY argument must be parameter-declaration-shaped (a type word or
    // `...`), not just one: a real prototype has no plain-identifier or
    // string-literal parameter. mingw intrin-impl.h's `__INTRINSICS_USEINLINE
    // __buildstos(__stosq, unsigned __int64, "q|q")` follows a decl head
    // (extern __inline__ ...) yet MUST expand — its first and third
    // arguments can never appear in a declarator's parameter list. The
    // any-arg reading suppressed it and the parse died at the unexpanded
    // macro name. (gcc always expands here; suppression is madc's deliberate
    // leniency for the SMAUG `#define bug(...)` + later-real-definition
    // pattern, so it must stay confined to arg lists gcc could reject.)
    for ( const std::string &arg : args )
    {
	size_t i = 0;
	while ( i < arg.size() && (arg[i] == ' ' || arg[i] == '\t') )
	    ++i;
	if ( i >= arg.size() )
	    continue;
	if ( arg.compare(i, 3, "...") == 0 )
	    continue;
	if ( !starts_with_type_word(arg) )
	    return false;
    }
    return true;
}

static bool macro_body_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

typedef Program::MacroDef::ReplacementToken MacroReplacementToken;

static bool pp_literal_start(const std::string &text, size_t pos,
			     size_t &quote, bool &raw)
{
    static const char *const raw_prefixes[] = { "u8R", "uR", "UR", "LR", "R" };
    static const char *const prefixes[] = { "u8", "u", "U", "L", "" };

    for ( size_t i = 0; i < sizeof(raw_prefixes) / sizeof(raw_prefixes[0]); ++i )
    {
	size_t n = strlen(raw_prefixes[i]);
	if ( text.compare(pos, n, raw_prefixes[i]) == 0
	  && pos + n < text.size() && text[pos + n] == '"' )
	{
	    quote = pos + n;
	    raw = true;
	    return true;
	}
    }
    for ( size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i )
    {
	size_t n = strlen(prefixes[i]);
	if ( text.compare(pos, n, prefixes[i]) != 0 || pos + n >= text.size() )
	    continue;
	char q = text[pos + n];
	if ( q == '"' || q == '\'' )
	{
	    quote = pos + n;
	    raw = false;
	    return true;
	}
    }
    return false;
}

static std::vector<MacroReplacementToken>
tokenize_macro_spelling(const std::string &text)
{
    typedef MacroReplacementToken T;
    std::vector<T> tokens;
    for ( size_t i = 0; i < text.size(); )
    {
	size_t start = i;
	if ( macro_body_space(text[i]) )
	{
	    while ( i < text.size() && macro_body_space(text[i]) )
		++i;
	    tokens.push_back(T(T::rtWhitespace, start, i));
	    continue;
	}
	if ( text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/' )
	{
	    i += 2;
	    while ( i < text.size() && text[i] != '\n' && text[i] != '\r' )
		++i;
	    tokens.push_back(T(T::rtComment, start, i));
	    continue;
	}
	if ( text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*' )
	{
	    i += 2;
	    while ( i < text.size()
		 && !(text[i - 1] == '*' && text[i] == '/') )
		++i;
	    if ( i < text.size() )
		++i;
	    tokens.push_back(T(T::rtComment, start, i));
	    continue;
	}

	size_t quote = 0;
	bool raw = false;
	if ( pp_literal_start(text, i, quote, raw) )
	{
	    if ( raw )
	    {
		size_t open = text.find('(', quote + 1);
		if ( open != std::string::npos && open - quote - 1 <= 16 )
		{
		    std::string close = ")" + text.substr(quote + 1,
							 open - quote - 1) + "\"";
		    size_t end = text.find(close, open + 1);
		    i = end == std::string::npos ? text.size() : end + close.size();
		}
		else
		    i = text.size();
	    }
	    else
	    {
		char q = text[quote];
		i = quote + 1;
		bool escaped = false;
		while ( i < text.size() )
		{
		    char c = text[i++];
		    if ( escaped )
			escaped = false;
		    else if ( c == '\\' )
			escaped = true;
		    else if ( c == q )
			break;
		}
	    }
	    tokens.push_back(T(T::rtLiteral, start, i));
	    continue;
	}
	if ( text[i] == '_' || isalpha((unsigned char)text[i]) )
	{
	    ++i;
	    while ( i < text.size()
		 && (text[i] == '_' || isalnum((unsigned char)text[i])) )
		++i;
	    tokens.push_back(T(T::rtIdentifier, start, i));
	    continue;
	}
	if ( isdigit((unsigned char)text[i])
	  || (text[i] == '.' && i + 1 < text.size()
	      && isdigit((unsigned char)text[i + 1])) )
	{
	    ++i;
	    while ( i < text.size() )
	    {
		char c = text[i];
		if ( c == '_' || c == '.' || c == '\''
		  || isalnum((unsigned char)c) )
		{
		    ++i;
		    continue;
		}
		if ( (c == '+' || c == '-') && i > start
		  && (text[i - 1] == 'e' || text[i - 1] == 'E'
		      || text[i - 1] == 'p' || text[i - 1] == 'P') )
		{
		    ++i;
		    continue;
		}
		break;
	    }
	    tokens.push_back(T(T::rtPpNumber, start, i));
	    continue;
	}
	if ( text[i] == '#' )
	{
	    if ( i + 1 < text.size() && text[i + 1] == '#' )
	    {
		i += 2;
		tokens.push_back(T(T::rtPaste, start, i));
	    }
	    else
	    {
		++i;
		tokens.push_back(T(T::rtHash, start, i));
	    }
	    continue;
	}
	++i;
	tokens.push_back(T(T::rtPunct, start, i));
    }
    return tokens;
}

static bool macro_token_space(const MacroReplacementToken &token)
{
    return token.kind == MacroReplacementToken::rtWhitespace
	|| token.kind == MacroReplacementToken::rtComment;
}

static std::string macro_token_text(const std::string &text,
				    const MacroReplacementToken &token)
{
    return text.substr(token.begin, token.end - token.begin);
}

static const std::vector<MacroReplacementToken> &
macro_replacement_tokens(const Program::MacroDef &macro)
{
    if ( macro.replacement_tokens_for != macro.body )
    {
	macro.replacement_tokens = tokenize_macro_spelling(macro.body);
	macro.replacement_tokens_for = macro.body;
    }
    return macro.replacement_tokens;
}

static bool macro_param_use_is_raw(const std::vector<MacroReplacementToken> &tokens,
				   size_t use)
{
    size_t left = use;
    while ( left > 0 && macro_token_space(tokens[left - 1]) )
	--left;
    if ( left > 0
	 && (tokens[left - 1].kind == MacroReplacementToken::rtHash
	     || tokens[left - 1].kind == MacroReplacementToken::rtPaste) )
	return true;

    size_t right = use + 1;
    while ( right < tokens.size() && macro_token_space(tokens[right]) )
	++right;
    return right < tokens.size()
	&& tokens[right].kind == MacroReplacementToken::rtPaste;
}

static bool macro_param_has_expanded_use(const Program::MacroDef &macro,
					 const std::string &param)
{
    const std::vector<MacroReplacementToken> &tokens =
	macro_replacement_tokens(macro);
    for ( size_t i = 0; i < tokens.size(); ++i )
	if ( tokens[i].kind == MacroReplacementToken::rtIdentifier
	  && macro_token_text(macro.body, tokens[i]) == param
	  && !macro_param_use_is_raw(tokens, i) )
	    return true;
    return false;
}

// The one token-to-source spelling owner lives with reconstruct_source below.
// Macro argument pre-expansion also needs it: its temporary token stream must
// round-trip literals without changing their value or type.
std::string madc_token_spelling(TokenBase *tb);  // fwd (the one spelling owner, defined below)

static std::string stringify_macro_arg(const std::string &raw)
{
    std::vector<MacroReplacementToken> tokens = tokenize_macro_spelling(raw);
    std::string out("\"");
    bool pending_space = false;
    bool wrote = false;
    for ( size_t i = 0; i < tokens.size(); ++i )
    {
	if ( macro_token_space(tokens[i]) )
	{
	    if ( wrote )
		pending_space = true;
	    continue;
	}
	if ( pending_space )
	{
	    out += ' ';
	    pending_space = false;
	}
	std::string spelling = macro_token_text(raw, tokens[i]);
	for ( size_t j = 0; j < spelling.size(); ++j )
	{
	    if ( spelling[j] == '"' || spelling[j] == '\\' )
		out += '\\';
	    out += spelling[j];
	}
	wrote = true;
    }
    out += '"';
    return out;
}

static size_t macro_fixed_param_count(const Program::MacroDef &macro)
{
    if ( macro.variadic && !macro.variadic_param.empty()
	 && !macro.params.empty() )
	return macro.params.size() - 1;
    return macro.params.size();
}

static std::string expand_function_macro_body(
	const Program::MacroDef &macro,
	const std::vector<std::string> &raw_args,
	const std::vector<std::string> &expanded_args)
{
    std::map<std::string, std::string> raw_params;
    std::map<std::string, std::string> expanded_params;
    size_t fixed = macro_fixed_param_count(macro);
    for ( size_t i = 0; i < fixed; ++i )
    {
	raw_params[macro.params[i]] = i < raw_args.size() ? raw_args[i] : "";
	expanded_params[macro.params[i]] =
	    i < expanded_args.size() ? expanded_args[i] : "";
    }
    if ( macro.variadic )
    {
	std::string raw_varargs;
	std::string expanded_varargs;
	for ( size_t i = fixed; i < raw_args.size(); ++i )
	{
	    if ( i > fixed )
	    {
		raw_varargs += ", ";
		expanded_varargs += ", ";
	    }
	    raw_varargs += raw_args[i];
	    expanded_varargs += i < expanded_args.size() ? expanded_args[i] : raw_args[i];
	}
	raw_params["__VA_ARGS__"] = raw_varargs;
	expanded_params["__VA_ARGS__"] = expanded_varargs;
	if ( !macro.variadic_param.empty() )
	{
	    raw_params[macro.variadic_param] = raw_varargs;
	    expanded_params[macro.variadic_param] = expanded_varargs;
	}
    }

    const std::vector<MacroReplacementToken> &tokens =
	macro_replacement_tokens(macro);
    std::string expanded;
    expanded.reserve(macro.body.size());
    // True immediately after a ## was consumed: the next token is the paste's
    // RIGHT operand. If that operand substitutes to EMPTY (a placemarker,
    // C11 6.10.3.3), the paste result is the left operand alone — a space
    // must keep the FOLLOWING body token from gluing to it in this
    // string-based expansion (`A ## B+` with empty B re-lexed `+`+`+` as
    // `++` — c-testsuite 00202).
    bool after_paste = false;
    for ( size_t i = 0; i < tokens.size(); )
    {
	const MacroReplacementToken &token = tokens[i];
	if ( token.kind == MacroReplacementToken::rtHash )
	{
	    size_t param = i + 1;
	    while ( param < tokens.size() && macro_token_space(tokens[param]) )
		++param;
	    if ( param < tokens.size()
	      && tokens[param].kind == MacroReplacementToken::rtIdentifier )
	    {
		std::string name = macro_token_text(macro.body, tokens[param]);
		std::map<std::string, std::string>::const_iterator raw =
		    raw_params.find(name);
		if ( raw != raw_params.end() )
		{
		    expanded += stringify_macro_arg(raw->second);
		    i = param + 1;
		    continue;
		}
	    }
	}
	if ( token.kind == MacroReplacementToken::rtPaste )
	{
	    while ( !expanded.empty() && macro_body_space(expanded.back()) )
		expanded.pop_back();
	    ++i;
	    while ( i < tokens.size() && macro_token_space(tokens[i]) )
		++i;
	    after_paste = true;
	    continue;
	}
	if ( token.kind == MacroReplacementToken::rtIdentifier )
	{
	    std::string name = macro_token_text(macro.body, token);
	    std::map<std::string, std::string>::const_iterator value =
		expanded_params.find(name);
	    if ( value != expanded_params.end() )
	    {
		const std::string &sub = macro_param_use_is_raw(tokens, i)
		    ? raw_params[name] : value->second;
		expanded += sub;
		if ( after_paste && sub.empty() )
		    expanded += ' ';	// placemarker: separate the next token
		after_paste = false;
		++i;
		continue;
	    }
	}
	after_paste = false;
	if ( token.kind == MacroReplacementToken::rtComment )
	    expanded += ' ';
	else
	    expanded += macro_token_text(macro.body, token);
	++i;
    }
    return expanded;
}

static std::string read_macro_body(Source &source)
{
    std::string body;

    enum LiteralMode { lmNone, lmString, lmCharacter };
    LiteralMode literal = lmNone;
    bool escaped = false;

    while ( source.good() && !source.eof() )
    {
	char ch = source.peek();
	if ( ch == '\n' || ch == '\r' )
	    break;
	if ( ch == '\\' )
	{
	    source.get();
	    char next = source.peek();
	    if ( next == '\n' || next == '\r' )
	    {
		source.get();
		if ( source.peek() == '\n' )
		    source.get();
		continue;
	    }
	    body += '\\';
	    if ( literal != lmNone )
		escaped = !escaped;
	    continue;
	}
	if ( literal != lmNone )
	{
	    body += source.get();
	    if ( escaped )
		escaped = false;
	    else if ( (literal == lmString && ch == '"')
		   || (literal == lmCharacter && ch == '\'') )
		literal = lmNone;
	    continue;
	}
	if ( ch == '"' || ch == '\'' )
	{
	    literal = ch == '"' ? lmString : lmCharacter;
	    escaped = false;
	    body += source.get();
	    continue;
	}
	if ( ch == '/' )
	{
	    source.get();
	    if ( source.peek() == '/' )
	    {
		source.get();
		while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		    source.get();
		break;
	    }
	    if ( source.peek() == '*' )
	    {
		source.get();
		while ( source.good() && !source.eof() )
		{
		    ch = source.get();
		    if ( ch == '*' && source.peek() == '/' )
		    {
			source.get();
			break;
		    }
		}
		if ( !body.empty() && body.back() != ' ' && body.back() != '\t' )
		    body += ' ';
		continue;
	    }
	    body += '/';
	    continue;
	}
	body += source.get();
    }

    while ( !body.empty() && (body.back() == ' ' || body.back() == '\t') )
	body.pop_back();
    return body;
}

// keyword tokens
TokenDO		tkDO;
TokenIF		tkIF;
TokenFOR	tkFOR;
TokenELSE	tkELSE;
TokenRETURN	tkRETURN;
TokenGOTO	tkGOTO;
TokenCASE	tkCASE;
TokenBREAK	tkBREAK;
TokenCONT	tkCONT;
TokenTRY	tkTRY;
TokenCATCH	tkCATCH;
TokenTHROW	tkTHROW;
TokenSWITCH	tkSWITCH;
TokenWHILE	tkWHILE;
TokenCLASS	tkCLASS;
TokenSTRUCT	tkSTRUCT;
TokenUNION	tkUNION;
TokenDEFAULT	tkDEFAULT;
TokenTYPEDEF	tkTYPEDEF;
TokenOPEROVER	tkOPEROVER;
TokenREGISTER	tkREGISTER;
TokenSTATIC	tkSTATIC;
TokenENUM	tkENUM;
TokenCONST	tkCONST;
TokenEXTERN	tkEXTERN;
TokenRESTRICT	tkRESTRICT;
TokenVOLATILE	tkVOLATILE;
TokenUSING	tkUSING;
TokenFRIEND	tkFRIEND;
TokenNAMESPACE	tkNAMESPACE;
TokenPREFER	tkPREFER;
TokenDEFER	tkDEFER;
TokenTEMPLATE	tkTEMPLATE;
TokenNEW	tkNEW;
TokenDELETE	tkDELETE;

// basic type tokens
TokenVOID	tkVOID;
TokenBOOL	tkBOOL;
TokenC23BOOL	tkC23BOOL;
TokenCHAR	tkCHAR;
TokenINT	tkINT;
TokenINT8	tkINT8;
TokenINT16	tkINT16;
TokenINT24	tkINT24;
TokenINT32	tkINT32;
TokenINT64	tkINT64;
TokenUINT8	tkUINT8;
TokenUINT16	tkUINT16;
TokenUINT24	tkUINT24;
TokenUINT32	tkUINT32;
TokenUINT64	tkUINT64;
TokenFLOAT	tkFLOAT;
TokenDOUBLE	tkDOUBLE;
TokenARRAY	tkARRAY;
TokenLPSTR	tkLPSTR;
TokenAUTO	tkAUTO;


// Fill the immutable (ROM) TokenRec from the now-formed pop-1 shell, so a fresh
// mutable (RAM) shell can be rebuilt from the rec alone (no-clone split, step 1).
// Provenance (file/line/column) and the ident spelling_id are already stamped by
// the time a token reaches the stream (getRealToken / getToken / the tokenize
// loop), so this just snapshots them into the POD record.
void Program::finalize_pop1_rec(TokenBase *tb)
{
    TokenRec &r = tb->rec;
    r.kind = (uint16_t)tb->id();
    if ( tb->is_real() )
    {
	double d = tb->dval();
	memcpy(&r.value, &d, sizeof(d));	// store the double's bits
    }
    else
	r.value = tb->get();			// _token / char code / int value
    // String payload (interning Step 4): identifiers/datatypes are interned at
    // creation (make_ident/make_datatype/the TokenIdent ctors). Only the CONTENT
    // subclasses retain a `str`, and only they need a fallback intern here (they
    // may be mutated after construction — e.g. wide-string conversion), so the POD
    // spelling_id reflects the final bytes (NUL-preserving via the std::string overload).
    if ( r.spelling_id == 0 )
    {
	TokenType tt = tb->type();
	if ( tt == TokenType::ttString )
	    r.spelling_id = strpool.intern(((TokenStr *)tb)->str);
	else if ( tt == TokenType::ttComment )
	    r.spelling_id = strpool.intern(((TokenREM *)tb)->str);
    }
    r.line = (int32_t)tb->line;
    r.column = (int32_t)tb->column;
    if ( tb->file )
    {
	if ( tb->file != _finalize_last_file )		// almost always the same file as the prior token
	{
	    _finalize_last_file = tb->file;
	    _finalize_last_file_id = strpool.intern(tb->file);
	}
	r.file_id = _finalize_last_file_id;
    }
    else
	r.file_id = 0;
}

void Program::push_token_with_literal_concat(TokenBase *tb)
{
    if ( tb->type() == TokenType::ttString
      && !tokens.empty()
      && tokens.back()->type() == TokenType::ttString )
    {
	TokenStr *prev = (TokenStr *)tokens.back();
	TokenStr *next = (TokenStr *)tb;
	if ( prev->wide || next->wide )
	{
	    if ( !prev->wide )
	    {
		prev->str = narrow_string_as_wide(prev->str);
		prev->wide = true;
	    }
	    if ( next->wide )
		prev->str += next->str;
	    else
		append_narrow_string_as_wide(prev->str, next->str);
	}
	else
	    prev->str += next->str;
	// the merged literal keeps every piece's SOURCE extent — the span
	// classifier paints one row per piece, not one mis-anchored blob.
	prev->src_pieces.insert(prev->src_pieces.end(),
				next->src_pieces.begin(),
				next->src_pieces.end());
	// the merged literal lives in `prev` (already in the stream); refresh its
	// rec spelling to the concatenated bytes so its ROM stays self-describing.
	prev->rec.spelling_id = strpool.intern(prev->str);
	// pin any queued #pragma pack ops to the SURVIVING token (tb dies here)
	pin_pending_pack_ops(prev);
	delete tb;
	return;
    }
    ++_tok_produced;	// --show-stats: a real stream token emitted by the lexer
    finalize_pop1_rec(tb);
    tokens.push_back(tb);
    if ( pack_protocol_unit )	// __need serving: the includer owns this token
	pack_protocol_token_owner[tb] = pack_protocol_unit;	// (identity-keyed)
    pin_pending_pack_ops(tb);
}

// Pin queued #pragma pack ops to the first real token emitted after the
// directive (see the pack side-channel block in madc.h); nextToken()
// applies them when that token is consumed.
void Program::pin_pending_pack_ops(TokenBase *tb)
{
    if ( _pending_pack_ops.empty() || !tb )
	return;
    std::vector<std::pair<int,int> > &ev = _pragma_pack_events[tb];
    ev.insert(ev.end(), _pending_pack_ops.begin(), _pending_pack_ops.end());
    _pending_pack_ops.clear();
}

// Predefined compiler macros captured at build time (scripts/gen_predefined_macros.sh
// -> src/predefined_macros.cpp). The struct shapes match the generated file.
struct MadcPredefObj  { const char *name; const char *value; };
struct MadcPredefFunc { const char *name; const char *params; const char *body; };
// Accessor functions (PLT-resolved) rather than direct extern data refs — keeps
// the PIE binary free of text relocations (see gen_predefined_macros.sh).
extern const MadcPredefObj  *madc_predefined_objects();
extern const MadcPredefFunc *madc_predefined_functions();

// Captured-predefine names that exist only in g++'s C++ modes (the capture
// runs g++; `gcc -std=cNN -dM` defines none of these). Skipped when the
// selected mode does not present as C++ — diff of `g++ -x c++ -dM` vs
// `gcc -std=c17 -dM` plus the __cpp_* feature-test family.
static bool predefine_is_cpp_only(const char *name)
{
    if ( strncmp(name, "__cpp_", 6) == 0 )
	return true;
    static const char *const cpp_only[] = {
	"__cplusplus", "__GNUG__", "_GNU_SOURCE", "__DEPRECATED",
	"__EXCEPTIONS", "__GLIBCXX_BITSIZE_INT_N_0", "__GLIBCXX_TYPE_INT_N_0",
	"__GXX_EXPERIMENTAL_CXX0X__", "__GXX_RTTI", "__GXX_WEAK__",
	"__STDCPP_DEFAULT_NEW_ALIGNMENT__", "__STDCPP_THREADS__", NULL
    };
    for ( int i = 0; cpp_only[i]; ++i )
	if ( strcmp(name, cpp_only[i]) == 0 )
	    return true;
    return false;
}

void Program::_tokenizer_init()
{
    // Bind the interned-spelling maps to this Program's intern_table BEFORE any
    // insert (add_keywords / add_datatypes / the predefined define_map below) so
    // their string-keyed inserts intern correctly; hot lookups in _getToken pass a
    // pre-computed spelling_id (uint32) instead of comparing strings.
    // Bind the active pool for TokenIdent::spelling() (interning Step 4). Compile is
    // sequential per-Program, so the currently-initializing Program owns the accessor.
    activate_token_pools();
    keyword_map.set_pool(&strpool);
    cpp_operator_map.set_pool(&strpool);
    define_map.set_pool(&strpool);
    macro_map.set_pool(&strpool);
    // datatype_map uses a DEDICATED dense pool (not strpool): type names are a
    // small domain, so a private pool keeps its intern_keyed_map _slot tight.
    datatype_map.set_pool(&type_name_pool);
    // Template-family maps share a dedicated dense template-name pool (same
    // _slot-sizing discipline as datatype_map; rung-1 substrate consolidation).
    partial_spec_map.set_pool(&template_name_pool);
    template_map.set_pool(&template_name_pool);
    template_alias_map.set_pool(&template_name_pool);
    var_template_map.set_pool(&template_name_pool);
    fn_template_decl_map.set_pool(&template_name_pool);
    fn_template_instantiated_vars.set_pool(&template_name_pool);
    fn_template_map.set_pool(&template_name_pool);
    namespace_datatype_map.set_pool(&namespace_name_pool);
    // Pre-size the value pools so they don't relocate as entries are added: a
    // single allocation instead of incremental growth, and — for
    // namespace_datatype_map, whose values are inner maps held by pointer
    // across inserts — it keeps those pointers stable (the std::map-node
    // analogue). Sizes are headroom for typical programs; growth past them is
    // still correct (find()-after-insert re-fetches where it matters).
    namespace_datatype_map.reserve(32);
    template_map.reserve(64);
    template_alias_map.reserve(32);
    partial_spec_map.reserve(32);
    var_template_map.reserve(32);
    fn_template_map.reserve(64);
    fn_template_decl_map.reserve(32);
    fn_template_instantiated_vars.reserve(64);

    tkProgram = NULL;
    tkFunction = NULL;
    try_depth = 0;
    _cur_token = NULL;
    _prv_token = NULL;
    // A fresh tokenize session: newly created tokens must not inherit the
    // ambient parse position of whatever unit ran before. getRealToken's
    // backstop stamps a token from THIS Program's source only when the
    // ctor stamp is 0 — a runtime-compile child (or a later --project TU)
    // otherwise keeps the previous unit's stale nonzero position on every
    // token (madcide IDE-3 found it: every child diagnostic carried the
    // HOST program's last line).
    TokenBase::_parse_file = NULL;
    TokenBase::_parse_line = 0;
    TokenBase::_parse_column = 0;
    deferred_function_body_sink = NULL;
    parsing_cpp_struct_class = false;
    _include_iostream = false;
    _include_stdio = false;
    _include_string = false;
    _include_ns_madc = false;
    _value_stream_operator_injected = false;
    auto_include_user_units.clear();
    included_files.clear();
    include_guard_by_file.clear();
    pending_auto_include_headers.clear();
    pending_auto_include_identifiers.clear();
    suppress_auto_include_scan = false;
    pending_no_strict_aliasing = false;
    while ( !_pack_stack.empty() )
	_pack_stack.pop();
    _pack_current = 0;
    _pending_pack_ops.clear();
    _pragma_pack_events.clear();
    add_keywords();
    add_datatypes();

    // C modes: `inline` and its GNU spellings stay ignored storage hints
    // (the C99 inline no-external-definition model is a different feature;
    // erasure keeps today's behavior). C++ modes / the madc dialect: `inline`
    // is a REAL reserved decl-specifier (registered in add_keywords like
    // constexpr) that carries vague linkage — the GNU spellings map to it.
    if ( !cpp_keyword_active(STD_CPP98) )
    {
	define_map["inline"] = "";
	define_map["__inline__"] = "";
	define_map["__inline"] = "";
    }
    else
    {
	define_map["__inline__"] = "inline";
	define_map["__inline"] = "inline";
    }
    // constexpr (slice 5) / consteval / constinit (slice 6) are real reserved
    // tokens, no longer erased: constexpr activates the TokenIF `if constexpr`
    // discard machinery, and all three are consumed as ignored decl-specifiers
    // (TokenCppKeyword::parse / the member-specifier loop /
    // is_ignored_cpp_specifier_token, which already recognize all three
    // spellings).
    // noexcept is NOT a plain empty define nor a function-like macro: the
    // macro path splits its argument on top-level commas, and the C
    // preprocessor does not treat <...> as grouping, so a template-id
    // condition like noexcept(is_nothrow_constructible<T, Args...>::value)
    // would split into two macro arguments ("Too many parameters"). It is
    // stripped by balanced-paren consumption in getToken() instead.
    define_map["__extension__"] = "";
    // _Alignas(N) (C11) / alignas(N) (C++11) are alignment specifiers — consume
    // like __attribute__. The lexer strips the specifier and its parens (or, when
    // the argument names a layout attribute, preserves it for the parser). Both
    // spellings map to the same path; libstdc++ uses the bare `alignas` keyword
    // (e.g. __aligned_membuf's `alignas(__alignof__(_Tp)) unsigned char ...`).
    define_map["_Alignas"] = "__attribute__";
    define_map["alignas"] = "__attribute__";
    define_map["__restrict"] = "";
    define_map["__restrict__"] = "";
    define_map["__signed__"] = "signed";
    define_map["__const"] = "const";
    define_map["__signed"] = "signed";
    define_map["__signed__"] = "signed";
    define_map["__const__"] = "const";
    // GCC floating-point limit macros
    define_map["__FLT_MAX__"] = "3.40282347e+38F";
    define_map["__FLT_MIN__"] = "1.17549435e-38F";
    define_map["__DBL_MAX__"] = "1.7976931348623157e+308";
    define_map["__DBL_MIN__"] = "2.2250738585072014e-308";
    define_map["__LDBL_MAX__"] = "1.7976931348623157e+308";
    define_map["__LDBL_MIN__"] = "2.2250738585072014e-308";
    define_map["__FLT_EPSILON__"] = "1.19209290e-7F";
    define_map["__DBL_EPSILON__"] = "2.2204460492503131e-16";
    define_map["__LDBL_EPSILON__"] = "2.2204460492503131e-16";
    define_map["__FLT_MANT_DIG__"] = "24";
    define_map["__DBL_MANT_DIG__"] = "53";
    define_map["__LDBL_MANT_DIG__"] = "53";
    define_map["__FLT_DIG__"] = "6";
    define_map["__DBL_DIG__"] = "15";
    define_map["__LDBL_DIG__"] = "15";
    define_map["__BIGGEST_ALIGNMENT__"] = "16";

    // GCC predefined macros for C compatibility
    define_map["__DATE__"] = "\"" __DATE__ "\"";
    define_map["__TIME__"] = "\"" __TIME__ "\"";
    // Built-in release-version string literal. MADC_VERSION_STR is supplied by
    // the build (-DMADC_VERSION_STR='"x.y.z"' from ../VERSION); the value stored
    // here keeps its quotes so the macro substitutes as a const char* literal.
#ifndef MADC_VERSION_STR
#define MADC_VERSION_STR "0.0.0"
#endif
    define_map["MADC_VERSION"] = "\"" MADC_VERSION_STR "\"";
    // madc's own compiler-identity macros — the peer-compiler equivalent of Clang's
    // __clang__. madc still impersonates the host gcc (it seeds the toolchain's whole
    // predefined set below, incl. __GNUC__) so UNMODIFIED libstdc++/glibc parse; this
    // ALSO declares madc's own identity so madc-aware code can detect it without
    // pretending to BE gcc. Defined in EVERY mode (like __clang__), unlike __cplusplus.
    define_map["__madc__"] = "1";
    define_map["__MADC__"] = "1";
    define_map["__MADC_VERSION__"] = "\"" MADC_VERSION_STR "\"";
    define_map["__CHAR_BIT__"] = "8";
    define_map["__SIZEOF_SHORT__"] = "2";
    define_map["__SIZEOF_INT__"] = "4";
    define_map["__SIZEOF_LONG__"] = "8";
    define_map["__SIZEOF_LONG_LONG__"] = "8";
    define_map["__SIZEOF_POINTER__"] = "8";
    define_map["__SIZEOF_FLOAT__"] = "4";
    define_map["__SIZEOF_DOUBLE__"] = "8";
    define_map["__INT_MAX__"] = "2147483647";
    define_map["__LONG_MAX__"] = "9223372036854775807L";
    define_map["__LONG_LONG_MAX__"] = "9223372036854775807LL";
    define_map["__INTMAX_MAX__"] = "9223372036854775807L";
    define_map["__UINTMAX_MAX__"] = "18446744073709551615UL";
    define_map["__SHRT_MAX__"] = "32767";
    define_map["__SCHAR_MAX__"] = "127";
    define_map["__PTRDIFF_TYPE__"] = "long";
    define_map["__PTRDIFF_MAX__"] = "9223372036854775807L";
    define_map["__SIZE_TYPE__"] = "unsigned long";
    define_map["__SIZE_MAX__"] = "18446744073709551615UL";
    define_map["__INTPTR_TYPE__"] = "long";
    define_map["__UINTPTR_TYPE__"] = "unsigned long";
    define_map["__UINT8_TYPE__"] = "unsigned char";
    define_map["__INT8_TYPE__"] = "char";
    define_map["__UINT16_TYPE__"] = "unsigned short";
    define_map["__INT16_TYPE__"] = "short";
    define_map["__UINT32_TYPE__"] = "unsigned int";
    define_map["__INT32_TYPE__"] = "int";
    define_map["__UINT64_TYPE__"] = "unsigned long";
    define_map["__INT64_TYPE__"] = "long";
    define_map["__WCHAR_TYPE__"] = "int";
    define_map["__WINT_TYPE__"] = "unsigned int";
    define_map["__SIG_ATOMIC_TYPE__"] = "int";
    define_map["__INTMAX_TYPE__"] = "long";
    define_map["__UINTMAX_TYPE__"] = "unsigned long";
    define_map["__INT_LEAST8_TYPE__"] = "char";
    define_map["__UINT_LEAST8_TYPE__"] = "unsigned char";
    define_map["__INT_LEAST16_TYPE__"] = "short";
    define_map["__UINT_LEAST16_TYPE__"] = "unsigned short";
    define_map["__INT_LEAST32_TYPE__"] = "int";
    define_map["__UINT_LEAST32_TYPE__"] = "unsigned int";
    define_map["__INT_LEAST64_TYPE__"] = "long";
    define_map["__UINT_LEAST64_TYPE__"] = "unsigned long";
    define_map["__INT_FAST8_TYPE__"] = "char";
    define_map["__UINT_FAST8_TYPE__"] = "unsigned char";
    define_map["__INT_FAST16_TYPE__"] = "long";
    define_map["__UINT_FAST16_TYPE__"] = "unsigned long";
    define_map["__INT_FAST32_TYPE__"] = "long";
    define_map["__UINT_FAST32_TYPE__"] = "unsigned long";
    define_map["__INT_FAST64_TYPE__"] = "long";
    define_map["__UINT_FAST64_TYPE__"] = "unsigned long";
    // __builtin_va_list is NOT a macro: it is a real compiler type
    // (Program::builtin_va_list_type — the SysV __madc_va_list_tag[1]
    // singleton) resolved via resolve_builtin_type_spelling, so
    // `typedef __builtin_va_list va_list;` gets the true array-of-struct
    // semantics a textual expansion can never express.
    define_map["__builtin_va_arg"] = "va_arg";
    define_map["__null"] = "0";
    // __builtin_va_start passes through to the CIR as a real c2mir intrinsic
    // call (cir_builder emits N_CALL(__builtin_va_start, ap); c2mir lowers it to
    // MIR_VA_START on the user's own va_list). It is intentionally NOT a macro:
    // the earlier `__ap = __va_args` master-copy expansion mis-set reg_save_area
    // in large frames. va_end stays a no-op (the stdarg.h va_end macro handles
    // it as `((void)(ap))`).
    // va_list is the real __madc_va_list_tag[1] array type, so va_end is a
    // no-op cast (an array lvalue cannot be assigned) and va_copy copies the
    // one element — the same bodies the embedded stdarg.h macros use.
    {
	MacroDef m;
	m.params = {"__ap"};
	m.body = "((void)(__ap))";
	macro_map["__builtin_va_end"] = m;
    }
    {
	MacroDef m;
	m.params = {"__dest", "__src"};
	m.body = "((__dest)[0] = (__src)[0])";
	macro_map["__builtin_va_copy"] = m;
    }
    // Report the GCC version that compiled madc itself.
    define_map["__GNUC__"] = std::to_string(__GNUC__);
    define_map["__GNUC_MINOR__"] = std::to_string(__GNUC_MINOR__);
    define_map["__GNUC_PATCHLEVEL__"] = std::to_string(__GNUC_PATCHLEVEL__);
    define_map["__x86_64__"] = "1";
    // __LP64__ follows the target data model: gcc defines it on Linux and
    // darwin, mingw never does — and because mingw doesn't, the baked
    // predefine capture cannot overwrite a stale seed on win64 the way it
    // fixes __SIZEOF_LONG__ and friends; an unconditional seed leaks LP64
    // into every served mingw header (task #46 rider).
    if ( !target_llp64() )
	define_map["__LP64__"] = "1";
    define_map["__BYTE_ORDER__"] = std::to_string(__BYTE_ORDER__);
    define_map["__ORDER_LITTLE_ENDIAN__"] = std::to_string(__ORDER_LITTLE_ENDIAN__);
    define_map["__ORDER_BIG_ENDIAN__"] = std::to_string(__ORDER_BIG_ENDIAN__);

    // GCC __builtin_* → libc function aliases.
    // Most GCC builtins have the same signature as their libc counterpart.
    define_map["__builtin_abort"] = "abort";
    define_map["__builtin_exit"] = "exit";
    define_map["__builtin_malloc"] = "malloc";
    define_map["__builtin_calloc"] = "calloc";
    define_map["__builtin_free"] = "free";
    define_map["__builtin_memcpy"] = "memcpy";
    define_map["__builtin_memset"] = "memset";
    define_map["__builtin_memcmp"] = "memcmp";
    define_map["__builtin_memmove"] = "memmove";
    define_map["__builtin_strcmp"] = "strcmp";
    define_map["__builtin_strncmp"] = "strncmp";
    define_map["__builtin_strcpy"] = "strcpy";
    define_map["__builtin_strncpy"] = "strncpy";
    define_map["__builtin_strlen"] = "strlen";
    define_map["__builtin_printf"] = "printf";
    define_map["__builtin_printf_unlocked"] = "printf_unlocked";
    define_map["__builtin_sprintf"] = "sprintf";
    define_map["__builtin_puts"] = "puts";
    define_map["__builtin_putchar"] = "putchar";
    define_map["__builtin_abs"] = "abs";
    define_map["__builtin_labs"] = "labs";
    // fabs family rows live in the C99 math table below.
    define_map["__builtin_trap"] = "abort";
    define_map["__builtin_memchr"] = "memchr";
    define_map["__builtin_mempcpy"] = "mempcpy";
    define_map["__builtin_strchr"] = "strchr";
    define_map["__builtin_strrchr"] = "strrchr";
    define_map["__builtin_rindex"] = "rindex";
    define_map["__builtin_strstr"] = "strstr";
    define_map["__builtin_index"] = "index";
    define_map["__builtin_strncat"] = "strncat";
    define_map["__builtin_strcat"] = "strcat";
    define_map["__builtin_strcspn"] = "strcspn";
    define_map["__builtin_strpbrk"] = "strpbrk";
    define_map["__builtin_strspn"] = "strspn";
    define_map["__builtin_snprintf"] = "snprintf";
    define_map["__builtin_vsprintf"] = "vsprintf";
    define_map["__builtin_vsnprintf"] = "vsnprintf";
    define_map["__builtin_vprintf"] = "vprintf";
    define_map["__builtin_vfprintf"] = "vfprintf";
    define_map["__builtin_fprintf"] = "fprintf";
    define_map["__builtin_fprintf_unlocked"] = "fprintf_unlocked";
    define_map["__builtin_fputc"] = "fputc";
    define_map["__builtin_fputs"] = "fputs";
    define_map["__builtin_fputs_unlocked"] = "fputs_unlocked";
    define_map["__builtin_fwrite"] = "fwrite";
    define_map["__builtin_sscanf"] = "sscanf";
    define_map["__builtin_fscanf"] = "fscanf";
    define_map["__builtin_realloc"] = "realloc";
    define_map["__builtin_alloca"] = "alloca";
    define_map["__builtin_bzero"] = "bzero";
    define_map["__builtin_bcopy"] = "bcopy";
    // C99 <math.h> family, table-driven over roots x {"", "f", "l"}:
    // libstdc++'s cmath inline wrappers call the __builtin_* forms (its
    // include_next <math.h> supplies the real prototypes, so the aliased
    // names type correctly). The previous ad-hoc singles grew one cmath
    // branch at a time and missed the whole *l family the win64 staged
    // 13.2.0 text calls ("__builtin_acosl undeclared" across every
    // cmath consumer); the complete family closes the class. The *l
    // RESOLUTION on win64 is the fork map's flavor-pin job
    // (mir-mingw-stdio.h class 3): ucrtbase lacks most *l exports and
    // mis-flavors the rest (MSVC 8-byte long double).
    // The root list itself lives in include/libc_signatures.h's owner, because
    // the parser expands the SAME list into return types for an undeclared call
    // (madc_libc_return_class). A second copy here is how five of these ended up
    // registered by hand, one bug report at a time, while fifty-one silently read
    // xmm0 out of rax.
    {
	const char *const *c99_math_roots = madc_libc_math_roots();
	for ( int i = 0; c99_math_roots[i]; ++i )
	{
	    std::string root = c99_math_roots[i];
	    define_map["__builtin_" + root] = root;
	    define_map["__builtin_" + root + "f"] = root + "f";
	    define_map["__builtin_" + root + "l"] = root + "l";
	}
    }
    define_map["__builtin_conj"] = "conj";
    define_map["__builtin_conjf"] = "conjf";
    define_map["__builtin_conjl"] = "conjl";
    define_map["__builtin_creal"] = "creal";
    define_map["__builtin_crealf"] = "crealf";
    define_map["__builtin_creall"] = "creall";
    define_map["__builtin_cimag"] = "cimag";
    define_map["__builtin_cimagf"] = "cimagf";
    define_map["__builtin_cimagl"] = "cimagl";
    define_map["__builtin_llabs"] = "llabs";
    define_map["__builtin_stpcpy"] = "stpcpy";
    define_map["__builtin_stpncpy"] = "stpncpy";
    define_map["__builtin_strdup"] = "strdup";
    define_map["__builtin_strndup"] = "strndup";
    define_map["__builtin_strnlen"] = "strnlen";
    // bswap builtins: these don't exist in glibc as functions, but the
    // GCC testsuite uses them. Map to helpers exported by the madc binary.
    define_map["__builtin_bswap16"] = "__madc_bswap16";
    define_map["__builtin_bswap32"] = "__madc_bswap32";
    define_map["__builtin_bswap64"] = "__madc_bswap64";
    define_map["__bswap_16"] = "__madc_bswap16";
    define_map["__bswap_32"] = "__madc_bswap32";

    // __builtin_add/sub/mul_overflow pass through to CIR as real c2mir
    // builtins (handled natively at every width incl. __int128); the old
    // __madc_* long-helper remap truncated wider-than-64-bit results.
    // The *_p predicate variants stay mapped (no c2mir builtin for them).
    define_map["__builtin_add_overflow_p"] = "__madc_add_overflow_p";
    define_map["__builtin_sub_overflow_p"] = "__madc_sub_overflow_p";
    define_map["__builtin_mul_overflow_p"] = "__madc_mul_overflow_p";

    // __builtin_expect(expr, val) is a branch-prediction hint. Preserve
    // evaluation of the expected-value operand for GCC torture cases
    // that attach side effects there, but return expr.
    {
	MacroDef m;
	m.params = {"__expr", "__val"};
	m.body = "((void)(__val), (__expr))";
	macro_map["__builtin_expect"] = m;
    }
    // __builtin_prefetch is a no-op hint
    {
	MacroDef m;
	m.params = {"__addr", "__rw", "__loc"};
	m.body = "((void)(__addr))";
	macro_map["__builtin_prefetch"] = m;
    }
    // __builtin_launder(p) IS p ([ptr.launder]/2 — same address, same type).
    // It exists only to stop the optimizer assuming a pointer still refers to
    // the object it was formed from; madc's IR makes no such provenance
    // assumption, so the identity is the whole implementation. The macro keeps
    // the operand's TYPE (a function would need the template), and evaluates it
    // once. has_builtin() reads macro_map, so this also answers libc++'s
    // __has_builtin query truthfully. Without it std::__launder's body called
    // an undefined __builtin_launder, its instantiation never registered, and
    // every std::launder<T> left an undefined __launder import (task #103).
    {
	MacroDef m;
	m.params = {"__p"};
	m.body = "(__p)";
	macro_map["__builtin_launder"] = m;
    }
    // FP classification builtins (type-generic compiler magic; real <math.h>
    // C++ regions call them directly). Lowered Tier-1 onto the REAL glibc
    // classification exports (__fpclassify*/__isnan*/__isinf*/__signbit*/
    // __finite*), dispatched by sizeof — float subnormals would misclassify
    // if promoted through a double-only helper. Statement-expr keeps the
    // operand single-evaluation, matching the builtin's contract.
    {
	MacroDef m;
	m.params = {"__a", "__b", "__c", "__d", "__e", "__x"};
	m.body = "({ __typeof__(__x) __madc_fcx = (__x); "
		 "int __madc_fc = (sizeof(__madc_fcx) == 4 ? __fpclassifyf(__madc_fcx) "
		 ": sizeof(__madc_fcx) == 8 ? __fpclassify(__madc_fcx) "
		 ": __fpclassifyl(__madc_fcx)); "
		 "__madc_fc == FP_NAN ? (__a) : __madc_fc == FP_INFINITE ? (__b) "
		 ": __madc_fc == FP_NORMAL ? (__c) : __madc_fc == FP_SUBNORMAL ? (__d) : (__e); })";
	macro_map["__builtin_fpclassify"] = m;
    }
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "({ __typeof__(__x) __madc_fcx = (__x); "
		 "sizeof(__madc_fcx) == 4 ? __isnanf(__madc_fcx) "
		 ": sizeof(__madc_fcx) == 8 ? __isnan(__madc_fcx) : __isnanl(__madc_fcx); })";
	macro_map["__builtin_isnan"] = m;
    }
    {
	// __isinf* return the SIGN (+1/-1) for infinities, 0 otherwise —
	// exactly __builtin_isinf_sign's contract.
	MacroDef m;
	m.params = {"__x"};
	m.body = "({ __typeof__(__x) __madc_fcx = (__x); "
		 "sizeof(__madc_fcx) == 4 ? __isinff(__madc_fcx) "
		 ": sizeof(__madc_fcx) == 8 ? __isinf(__madc_fcx) : __isinfl(__madc_fcx); })";
	macro_map["__builtin_isinf_sign"] = m;
    }
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "({ __typeof__(__x) __madc_fcx = (__x); "
		 "sizeof(__madc_fcx) == 4 ? __finitef(__madc_fcx) "
		 ": sizeof(__madc_fcx) == 8 ? __finite(__madc_fcx) : __finitel(__madc_fcx); })";
	macro_map["__builtin_isfinite"] = m;
    }
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "({ __typeof__(__x) __madc_fcx = (__x); "
		 "int __madc_fc = (sizeof(__madc_fcx) == 4 ? __fpclassifyf(__madc_fcx) "
		 ": sizeof(__madc_fcx) == 8 ? __fpclassify(__madc_fcx) "
		 ": __fpclassifyl(__madc_fcx)); __madc_fc == FP_NORMAL; })";
	macro_map["__builtin_isnormal"] = m;
    }
    // (The IEEE quiet-comparison builtin family — isgreater/isless/
    // isunordered/… — is defined further below; quiet `<`/`>` are already
    // their correct lowering.)
    // __builtin_constant_p(expr) — always return 0 (not a constant)
    {
	MacroDef m;
	m.params = {"__expr"};
	m.body = "0";
	macro_map["__builtin_constant_p"] = m;
    }
    // __builtin_choose_expr(cond, true_expr, false_expr) chooses by a
    // compile-time integer condition. The condition is already reduced
    // for the current GCC execute-suite use by __builtin_constant_p.
    {
	MacroDef m;
	m.params = {"__cond", "__true_expr", "__false_expr"};
	m.body = "((__cond) ? (__true_expr) : (__false_expr))";
	macro_map["__builtin_choose_expr"] = m;
    }
    // __builtin_return_address(level) — unsupported, but many GCC
    // torture tests only need a stable sentinel value.
    {
	MacroDef m;
	m.params = {"__level"};
	m.body = "0";
	macro_map["__builtin_return_address"] = m;
    }
    // __builtin_unreachable() — map to abort()
    define_map["__builtin_unreachable"] = "abort";
    // __builtin_signbit(x) — check sign bit with 1/x trick.
    // Cannot use (x < 0.0) because -0.0 < 0.0 is false in IEEE 754.
    // 1.0/(-0.0) = -inf < 0.0 → true; 1.0/(+0.0) = +inf < 0.0 → false.
    // For ±normal/subnormal values, x < 0.0 suffices.
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "((__x) < 0.0 || 1.0 / (__x) < 0.0)";
	macro_map["__builtin_signbit"] = m;
	macro_map["__builtin_signbitf"] = m;
	macro_map["__builtin_signbitl"] = m;
    }
    // IEEE floating-point comparison builtins.
    {
	MacroDef unordered;
	unordered.params = {"__a", "__b"};
	unordered.body = "(((__a) != (__a)) || ((__b) != (__b)))";
	macro_map["__builtin_isunordered"] = unordered;
    }
    {
	MacroDef lessgreater;
	lessgreater.params = {"__a", "__b"};
	lessgreater.body = "(((__a) < (__b)) || ((__a) > (__b)))";
	macro_map["__builtin_islessgreater"] = lessgreater;
    }
    {
	MacroDef greater;
	greater.params = {"__a", "__b"};
	greater.body = "((__a) > (__b))";
	macro_map["__builtin_isgreater"] = greater;
    }
    {
	MacroDef greaterequal;
	greaterequal.params = {"__a", "__b"};
	greaterequal.body = "((__a) >= (__b))";
	macro_map["__builtin_isgreaterequal"] = greaterequal;
    }
    {
	MacroDef less;
	less.params = {"__a", "__b"};
	less.body = "((__a) < (__b))";
	macro_map["__builtin_isless"] = less;
    }
    {
	MacroDef lessequal;
	lessequal.params = {"__a", "__b"};
	lessequal.body = "((__a) <= (__b))";
	macro_map["__builtin_islessequal"] = lessequal;
    }
    {
	MacroDef inf;
	inf.body = "(1.0 / 0.0)";
	macro_map["__builtin_inf"] = inf;
	macro_map["__builtin_huge_val"] = inf;
	MacroDef inff;
	inff.body = "(1.0f / 0.0f)";
	macro_map["__builtin_inff"] = inff;
	macro_map["__builtin_huge_valf"] = inff;
	MacroDef infl;
	infl.body = "(1.0L / 0.0L)";
	macro_map["__builtin_infl"] = infl;
	macro_map["__builtin_huge_vall"] = infl;
    }
    {
	MacroDef nan;
	nan.params = {"__tag"};
	nan.body = "(0.0 / 0.0)";
	macro_map["__builtin_nan"] = nan;
	MacroDef nanf;
	nanf.params = {"__tag"};
	nanf.body = "(0.0f / 0.0f)";
	macro_map["__builtin_nanf"] = nanf;
	MacroDef nanl;
	nanl.params = {"__tag"};
	nanl.body = "(0.0L / 0.0L)";
	macro_map["__builtin_nanl"] = nanl;
    }
    {
	// SIGNALING NaN — deliberately not a copy of the quiet trio above.
	// 0.0/0.0 yields a QUIET NaN (it folds to the canon pattern bit-for-bit,
	// which is why the block above is correct), and no arithmetic can
	// produce a signaling one: every operation on a signaling NaN quiets it.
	// So the pattern is materialized directly. Aliasing these to the quiet
	// forms would compile and then make numeric_limits<T>::signaling_NaN()
	// silently wrong — libc++'s <limits> is what reaches them.
	//
	// Patterns are gcc's and clang's, verified identical on x86-64:
	//   nansf 7fa00000   nans 7ff4000000000000   nansl 7fff:a000000000000000
	// A statement expression rather than a C11 compound literal purely
	// because that is what madc parses today (an anonymous-union compound
	// literal is a separate front-end gap); c2mir supports statement
	// expressions natively and both canon compilers accept them.
	MacroDef nans;
	nans.params = {"__tag"};
	nans.body = "(__extension__ ({ union { unsigned long long __i; double __d; } __u;"
		    " __u.__i = 0x7ff4000000000000ULL; __u.__d; }))";
	macro_map["__builtin_nans"] = nans;
	MacroDef nansf;
	nansf.params = {"__tag"};
	nansf.body = "(__extension__ ({ union { unsigned int __i; float __f; } __u;"
		     " __u.__i = 0x7fa00000U; __u.__f; }))";
	macro_map["__builtin_nansf"] = nansf;
	// x87 80-bit: mantissa in the low 8 bytes, sign+exponent in the next 2,
	// so BOTH halves must be written — a union over a single 64-bit field
	// would leave the exponent bytes uninitialized now that `long double` is
	// genuinely 16 bytes wide. (It read as the double pattern plus stack
	// garbage while long double was still mapped to double.)
	MacroDef nansl;
	nansl.params = {"__tag"};
	nansl.body = "(__extension__ ({ union { unsigned long long __i[2]; long double __l; } __u;"
		     " __u.__i[0] = 0xa000000000000000ULL; __u.__i[1] = 0x7fffULL;"
		     " __u.__l; }))";
	macro_map["__builtin_nansl"] = nansl;
    }
    {
	MacroDef isnan;
	isnan.params = {"__x"};
	isnan.body = "((__x) != (__x))";
	macro_map["__builtin_isnan"] = isnan;
	macro_map["__builtin_isnanf"] = isnan;
	macro_map["__builtin_isnanl"] = isnan;
    }
    {
	MacroDef isfinite;
	isfinite.params = {"__x"};
	isfinite.body = "(((__x) == (__x)) && ((__x) != __builtin_inf()) && ((__x) != -__builtin_inf()))";
	macro_map["__builtin_isfinite"] = isfinite;
	macro_map["__builtin_isfinitef"] = isfinite;
	macro_map["__builtin_isfinitel"] = isfinite;
    }
    {
	MacroDef isinf;
	isinf.params = {"__x"};
	isinf.body = "((__x) == __builtin_inf() ? 1 : ((__x) == -__builtin_inf() ? -1 : 0))";
	macro_map["__builtin_isinf"] = isinf;
	macro_map["__builtin_isinff"] = isinf;
	macro_map["__builtin_isinfl"] = isinf;
    }
    // __builtin_classify_type is a PARSER builtin (parser.cpp expression
    // dispatch) — it must see the operand's type, which no macro can.
    // __builtin_alloca → alloca (line 930); compiler handles via stack bump pool.
    define_map["__builtin_ffs"] = "__madc_ffs";
    define_map["__builtin_ffsl"] = "__madc_ffsl";
    define_map["__builtin_ffsll"] = "__madc_ffsll";
    define_map["__builtin_clz"] = "__madc_clz";
    define_map["__builtin_clzl"] = "__madc_clzl";
    define_map["__builtin_clzll"] = "__madc_clzll";
    define_map["__builtin_ctz"] = "__madc_ctz";
    define_map["__builtin_ctzl"] = "__madc_ctzl";
    define_map["__builtin_ctzll"] = "__madc_ctzll";
    define_map["__builtin_clrsb"] = "__madc_clrsb";
    define_map["__builtin_clrsbl"] = "__madc_clrsbl";
    define_map["__builtin_clrsbll"] = "__madc_clrsbll";
    define_map["__builtin_popcount"] = "__madc_popcount";
    define_map["__builtin_popcountl"] = "__madc_popcountl";
    define_map["__builtin_popcountll"] = "__madc_popcountll";
    define_map["__builtin_parity"] = "__madc_parity";
    define_map["__builtin_parityl"] = "__madc_parityl";
    define_map["__builtin_parityll"] = "__madc_parityll";

    // Compiler builtins with no libc equivalent — map to __madc_builtin_* wrappers
    define_map["__builtin_object_size"] = "__madc_builtin_object_size";
    define_map["__builtin___strcpy_chk"] = "__madc_builtin_strcpy_chk";
    define_map["__builtin___stpcpy_chk"] = "__madc_builtin_stpcpy_chk";
    define_map["__builtin___stpncpy_chk"] = "__madc_builtin_stpncpy_chk";
    define_map["__builtin___strcat_chk"] = "__madc_builtin_strcat_chk";
    define_map["__builtin___strncpy_chk"] = "__madc_builtin_strncpy_chk";
    define_map["__builtin___strncat_chk"] = "__madc_builtin_strncat_chk";
    define_map["__builtin___memcpy_chk"] = "__madc_builtin_memcpy_chk";
    define_map["__builtin___memmove_chk"] = "__madc_builtin_memmove_chk";
    define_map["__builtin___mempcpy_chk"] = "__madc_builtin_mempcpy_chk";
    define_map["__builtin___memset_chk"] = "__madc_builtin_memset_chk";
    define_map["__builtin_frame_address"] = "__madc_builtin_frame_address";
    define_map["__builtin_setjmp"] = "__madc_builtin_setjmp";
    define_map["__builtin_longjmp"] = "__madc_builtin_longjmp_val";
    define_map["__builtin_uabs"] = "__madc_builtin_uabs";
    define_map["__builtin_umaxabs"] = "__madc_builtin_umaxabs";

    // __builtin_offsetof(type, member) — compute struct member offset.
    // Implemented as a function-like macro using the null-pointer trick.
    {
	MacroDef m;
	m.params = {"__type", "__member"};
	m.body = "((long)&((__type *)0)->__member)";
	macro_map["__builtin_offsetof"] = m;
    }

    {
	MacroDef m;
	m.body = "0";
	macro_map["__builtin_is_constant_evaluated"] = m;
    }

    // Predefined compiler macros captured at build time (gen_predefined_macros.sh).
    // Seeded AFTER the hand-set builtins (so the real toolchain values win) and
    // BEFORE -D (so -D can override, matching gcc). Real system headers branch on
    // these. C++-only macros follow presents_as_cpp(): the madc dialect and
    // every explicit C++ std impersonate g++ (so real libstdc++ headers parse);
    // explicit C modes stay plain gcc — the capture ran g++, but `gcc -std=cNN
    // -dM` defines NONE of these, and real glibc headers branch on them
    // (_GNU_SOURCE selects the transparent-union __SOCKADDR_ARG bind/accept
    // signatures; C code's `#ifdef __cplusplus` regions must stay off).
    for ( const MadcPredefObj *o = madc_predefined_objects(); o->name; ++o )
    {
	if ( !presents_as_cpp() && predefine_is_cpp_only(o->name) )
	    continue;
	// The capture ran a STRICT-std g++; strictness follows the SELECTED
	// mode (gcc parity: plain gcc/g++ default to the gnu dialects and
	// define no __STRICT_ANSI__ — glibc's features.h would otherwise
	// suppress _DEFAULT_SOURCE and hide timercmp-class declarations).
	if ( strcmp(o->name, "__STRICT_ANSI__") == 0 && !strict_ansi_mode() )
	    continue;
	// __cplusplus tracks the SELECTED --std=, not the value captured at
	// build time (the capture ran the host g++ at one fixed std; a pinned
	// 201703L made every `#if __cplusplus > 201703L` header region —
	// all of <compare>, the C++20 surface of <concepts>/<ranges> —
	// silently preprocess away under --std=c++20).
	if ( strcmp(o->name, "__cplusplus") == 0 )
	{
	    define_map[o->name] = cplusplus_value_for_std();
	    continue;
	}
	define_map[o->name] = o->value;
    }
    // C modes define __STDC_VERSION__ per the selected standard (gcc parity;
    // the g++-run capture cannot supply it, and glibc gates its C99/C11
    // surfaces — __USE_ISOC99/__USE_ISOC11 — on it). c89/c90 predate the
    // macro and leave it undefined, like gcc -std=c89.
    if ( is_c_mode() )
    {
	if ( const char *sv = stdc_version_for_std() )
	    define_map["__STDC_VERSION__"] = sv;
    }
    // madc's own version identity, #if-testable: MADC_VERSION expands to the
    // build's version as a string literal. sys.version (<ns_madc>) is the
    // runtime spelling of the same value.
    define_map["MADC_VERSION"] = std::string("\"") + MADC_VERSION_STR + "\"";
    // Compiler feature-test macros madc provides itself, gated by the std
    // floor (the build-time capture can't know them — they describe THIS
    // compiler's features, not the host's). <compare> requires
    // __cpp_impl_three_way_comparison >= 201907L to expose the comparison
    // category types, and includes <concepts>, whose body gates on
    // __cpp_concepts and carries the <type_traits> -> bits/c++config.h
    // chain every standalone libstdc++ include relies on.
    if ( is_cpp_mode() && language_std >= STD_CPP20 )
    {
	define_map["__cpp_impl_three_way_comparison"] = "201907L";
	define_map["__cpp_concepts"] = "202002L";
    }
    for ( const MadcPredefFunc *f = madc_predefined_functions(); f->name; ++f )
    {
	MacroDef m;
	std::string ps = f->params;
	size_t start = 0;
	while ( start <= ps.size() )
	{
	    size_t comma = ps.find(',', start);
	    std::string p = ps.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
	    while ( !p.empty() && p.front() == ' ' ) p.erase(p.begin());
	    while ( !p.empty() && p.back() == ' ' ) p.pop_back();
	    if ( !p.empty() ) m.params.push_back(p);
	    if ( comma == std::string::npos ) break;
	    start = comma + 1;
	}
	m.body = f->body;
	macro_map[f->name] = m;
    }

    // Command-line -D defines, applied AFTER the builtins/predefined so a -D
    // overrides one (matching gcc). Object-like only: -DNAME=VALUE / -DNAME (=> "1").
    for ( const std::pair<std::string,std::string> &d : cli_defines )
    {
	define_map[d.first] = d.second;
	note_std_abi_define(d.first, d.second);
    }
}

bool Program::include_already_seen(const std::string &path)
{
    return included_files.count(path) != 0;
}

std::string Program::current_source_directory()
{
    // host_path_dirname: on a Windows host the user spells the source
    // path with '\' (cmd/PowerShell tab-completion), and a bare rfind('/')
    // returned "" — every relative #include then resolved from cwd.
    return madc::detail::host_path_dirname(source.fname());
}

// Phase 6 (--forest-bind): open the grove container once, on the first system
// #include. With an explicit --forest-bind=PATH that path is the whole search;
// otherwise the carrier DISCOVERY CHAIN below probes the ordered arms
// (forest-carriers plan: one format, one loader, N carriers). A chain that
// ends empty applies registration_policy.forest_missing_policy — the dev
// default live-parses silently. The winning mapping is never unmapped: the
// forest reads from it for the process lifetime.
// --show-stats wall clock (R0 startup instrumentation).
static double forest_stat_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

// One discovery arm: map `path` (NULL = the running self-image: ELF trailer /
// Mach-O __MADC,__forest section) and open it as a forest container. Returns
// the opened forest, or NULL to fall through to the next arm. quiet_missing
// silences ONLY the not-a-container case — pass true where an absent blob is
// normal (the self-image of an unpacked dev binary), false where a concrete
// file was found and junk warrants the notice (sidecar / env / explicit
// path). An absent FILE never maps and always skips silently; a version-pin
// or corruption reject always prints (open_header); a producer-config
// (std / -D) mismatch always skips silently — the v27 multi-dialect contract
// (a C compile must not bind a C++-parsed corpus; the packed-binary default
// relies on it). The config gate reads only the directory header, so a
// mismatched arm costs footer + directory, not the pool/arena binds (R1).
// header_only stops right there, at the directory: the container-global
// segments (the AOT ledger) live outside the groves, so a consumer that only
// wants those must not be made to bind the frozen string pool / arena into
// live parse state it does not have (the .o link lane never parses).
static CirFrozenForest *forest_probe_arm(Program *prog, const char *path,
					 const char *arm, bool quiet_missing,
					 bool &config_mismatch,
					 double &map_secs, double &open_secs,
					 Program::ForestConfigMatch config_match,
					 bool header_only = false)
{
    double _t0 = forest_stat_now();
    const void *image = NULL;
    size_t image_len = 0;
    if ( !cir_forest_map_image(path, image, image_len) )
    {
	map_secs += forest_stat_now() - _t0;
	DBG(std::cout << "forest-bind: [" << arm << "] no image at "
	    << (path ? path : "self-image") << std::endl);
	return NULL;
    }
    map_secs += forest_stat_now() - _t0;
    size_t candidate_len = image_len;
    for (;;)
    {
	CirFrozenForest *f = new CirFrozenForest();
	double _t1 = forest_stat_now();
	if ( !f->open_header(image, candidate_len, quiet_missing) )
	{
	    open_secs += forest_stat_now() - _t1;
	    delete f;
	    return NULL;
	}
	size_t previous_len = 0;
	const bool have_previous = f->previous_image_len(previous_len);
	const uint32_t producer_config = f->language_std();
	const uint32_t consumer_config = madc_forest_config_word(prog);
	bool matches = config_match == Program::forestMatchAny;
	if ( config_match == Program::forestMatchExact )
	    matches = producer_config == consumer_config
		   && f->defines_hash() == madc_forest_defines_hash(prog);
	else if ( config_match == Program::forestMatchStdlibFlavor )
	    matches = (producer_config & CIR_FOREST_CONFIG_STDLIB_MASK)
		   == (consumer_config & CIR_FOREST_CONFIG_STDLIB_MASK);

	if ( !matches )
	{
	    open_secs += forest_stat_now() - _t1;
	    DBG(std::cout << "forest-bind: [" << arm << "] producer config"
		" mismatch — trying older profile" << std::endl);
	    if ( config_match == Program::forestMatchExact )
		config_mismatch = true;
	    delete f;
	}
	else if ( header_only || f->complete_open(/*c2m=*/NULL) )
	{
	    open_secs += forest_stat_now() - _t1;
	    DBG(std::cout << "forest-bind: [" << arm << "] opened container ("
		<< (header_only ? "directory only"
				: std::to_string(f->unit_count()) + " units") << ")"
		<< std::endl);
	    return f;
	}
	else
	{
	    open_secs += forest_stat_now() - _t1;
	    delete f;
	}

	if ( !have_previous )
	    return NULL;
	candidate_len = previous_len;
    }
}

// The ordered carrier discovery chain (forest-carriers plan: one format, one
// loader, N carriers). Returns the first arm that yields a usable container,
// or NULL. All three consumers walk it: the grove bind requires the exact
// producer config; raw source requires the selected stdlib flavor but is
// re-tokenized under the live std/-D policy; the AOT ledger is dialect-
// agnostic. Each arm may carry a newest-first stack of profiles. Keeping ONE
// walker keeps both arm order and within-arm profile order in one place.
CirFrozenForest *Program::probe_forest_chain(ForestConfigMatch config_match,
					     bool &cfg_mismatch,
					     bool header_only)
{
    // Arm 0: an explicit --forest-bind=PATH is the whole search (CLI beats
    // every discovery arm).
    if ( !forest_bind_path.empty() )
	return forest_probe_arm(this, forest_bind_path.c_str(),
				"explicit", /*quiet_missing=*/false,
				cfg_mismatch, _forest_map_seconds,
				_forest_open_seconds, config_match,
				header_only);

    // Arm 1: self-image — the running binary's own carrier (ELF trailer /
    // Mach-O __MADC,__forest section). An unpacked dev binary misses quietly.
    CirFrozenForest *f = forest_probe_arm(this, NULL, "self-image",
					  /*quiet_missing=*/true, cfg_mismatch,
					  _forest_map_seconds, _forest_open_seconds,
					  config_match,
					  header_only);
    // Arm 2 (forest-carriers S4): library image — in the SHARED shape the
    // forest rides libmadc's own image, so ONE container serves the thin CLI
    // and every embedding host. dladdr on a libmadc symbol resolves the image
    // path; in the monolithic shape that path IS the executable, which arm 1
    // already probed, so the arm is skipped. Deliberately NOT gated by
    // enable_external_forest: the library is part of the installation the host
    // already loaded and executes — not an external redirection of where
    // frozen state is loaded from.
    std::string exe = madc_self_exe_path();
    std::string lib = madc_self_lib_path();
    bool have_lib_image = !lib.empty() && lib != exe;
    if ( !f && have_lib_image )
	f = forest_probe_arm(this, lib.c_str(), "library-image",
			     /*quiet_missing=*/true, cfg_mismatch,
			     _forest_map_seconds, _forest_open_seconds,
			     config_match,
			     header_only);
    if ( !f && registration_policy.enable_external_forest )
    {
	// Arm 3: sidecar container beside an image (<exe>.forest, then
	// <lib>.forest in the shared shape — the packaged-install shape; dev
	// iteration without re-packing). Same image order as the carriers
	// above: the executable's sidecar outranks the library's.
	if ( !exe.empty() )
	    f = forest_probe_arm(this, (exe + ".forest").c_str(),
				 "sidecar", /*quiet_missing=*/false,
				 cfg_mismatch, _forest_map_seconds,
				 _forest_open_seconds, config_match,
				 header_only);
	if ( !f && have_lib_image )
	    f = forest_probe_arm(this, (lib + ".forest").c_str(),
				 "lib-sidecar", /*quiet_missing=*/false,
				 cfg_mismatch, _forest_map_seconds,
				 _forest_open_seconds, config_match,
				 header_only);
	// Arm 4: MADC_FOREST environment variable (a configured path; env
	// outranks the madc.ini key per the config precedence rule
	// CLI > environment > madc.ini > baked defaults).
	const char *env = getenv("MADC_FOREST");
	if ( !f && env && *env )
	    f = forest_probe_arm(this, env, "MADC_FOREST",
				 /*quiet_missing=*/false, cfg_mismatch,
				 _forest_map_seconds, _forest_open_seconds,
				 config_match,
				 header_only);
	// Arm 5 (forest-carriers S6): the madc.ini `forest =` key — the LAST
	// arm, because a configured default must lose to everything more
	// specific. The CLI parses the file and hands the path down through
	// the registration policy; libmadc hosts leave it empty.
	if ( !f && !registration_policy.forest_config_path.empty() )
	    f = forest_probe_arm(this,
				 registration_policy.forest_config_path.c_str(),
				 "madc.ini", /*quiet_missing=*/false,
				 cfg_mismatch, _forest_map_seconds,
				 _forest_open_seconds, config_match,
				 header_only);
    }
    return f;
}

// The arms the discovery chain actually probed, for the failure diagnostics
// below — ONE owner, so the loud notice and the strict error can never drift
// from each other or from probe_forest_chain's real arm list.
std::string Program::forest_probed_arms() const
{
    std::string arms("self-image, library image, <exe>.forest / <lib>.forest"
		     " sidecars, $MADC_FOREST");
    if ( !registration_policy.forest_config_path.empty() )
	arms += ", the madc.ini forest key";
    return arms;
}

CirFrozenForest *Program::ensure_bind_forest()
{
    if ( bind_forest_tried )
	return bind_forest;
    bind_forest_tried = true;

    // task #25 slice D: probe/map/open under the forest-work clock; a bound
    // forest gets the clock installed so its own on-demand entries (unit
    // loads, materialize, template payload) accumulate on the same clock.
    ForestWorkFrame _fw(&_forest_work_seconds, &_forest_work_depth);
    // S3 (R4-lite): a sibling TU adopts the group's already-bound instance
    // for this dialect — the (config word, -D hash) pair IS the probe's own
    // config gate, so a group hit is exactly what the probe would re-open.
    // The clock pointers re-install per adopter (sequential compile: the
    // forest's on-demand work accumulates on whoever is currently compiling).
    std::pair<uint32_t, uint32_t> group_key(0, 0);
    if ( compile_group )
    {
	group_key = std::make_pair(madc_forest_config_word(this),
				   madc_forest_defines_hash(this));
	std::map<std::pair<uint32_t, uint32_t>,
		 std::shared_ptr<CirFrozenForest> >::iterator gi =
	    compile_group->bind_forests.find(group_key);
	if ( gi != compile_group->bind_forests.end() )
	{
	    _bind_forest_holder = gi->second;
	    bind_forest = _bind_forest_holder.get();
	    bind_forest->_work_secs = &_forest_work_seconds;
	    bind_forest->_work_depth = &_forest_work_depth;
	    return bind_forest;
	}
    }
    bool cfg_mismatch = false;
    bind_forest = probe_forest_chain(forestMatchExact, cfg_mismatch);
    if ( bind_forest )
    {
	_bind_forest_holder.reset(bind_forest);
	if ( compile_group )
	    compile_group->bind_forests[group_key] = _bind_forest_holder;
	bind_forest->_work_secs = &_forest_work_seconds;
	bind_forest->_work_depth = &_forest_work_depth;
	return bind_forest;
    }

    // A named container that fails to open falls back to live parse LOUDLY
    // regardless of policy — the user pointed at it, so ignoring it silently
    // is exactly the degradation the failure policy exists to prevent (strict
    // still hard-errors).
    if ( !forest_bind_path.empty() )
    {
	if ( registration_policy.forest_missing_policy
	     == RegistrationPolicy::ForestPolicy::strict_require )
	    Throw(NULL) << "frozen forest required (strict forest policy):"
			   " '" << forest_bind_path << "' did not provide"
			   " a usable container" << std::flush;
	(error_stream ? *error_stream : std::cerr)
	    << "madc: --forest-bind: '" << forest_bind_path
	    << "' did not provide a usable forest container; falling"
	       " back to live parse" << std::endl;
	return NULL;
    }
    forest_missing_fallback(cfg_mismatch);
    return bind_forest;
}

// The carrier the AOT ledger is read from (-static-libmadc, S5). The grove
// bind's container when it opened one — same file, already mapped — else the
// SAME chain walked with the producer-config gate OFF, because a compile whose
// dialect cannot bind the groves must still be able to link madc's runtime.
// enable_forest_bind is likewise not consulted: --no-forest-bind says "parse
// the headers live", not "leave the runtime out of my binary".
// Silent on failure: the emit lane owns the diagnostic (it alone knows whether
// this program needs the runtime at all).
CirFrozenForest *Program::ensure_ledger_forest()
{
    if ( ledger_forest_tried )
	return ledger_forest;
    ledger_forest_tried = true;
    if ( bind_forest )
    {
	ledger_forest = bind_forest;
	return ledger_forest;
    }
    bool cfg_mismatch = false;
    // header_only: the ledger is a container-GLOBAL segment, so the directory
    // is the whole requirement — binding the frozen string pool and arena is
    // grove work, and it needs live parse state this consumer may not have
    // (the .o link lane links precompiled objects: no lexer, no pool).
    ledger_forest = probe_forest_chain(forestMatchAny,
				       cfg_mismatch, /*header_only=*/true);
    DBG(std::cout << "ledger: carrier "
	<< (ledger_forest ? "opened" : "not found") << std::endl);
    return ledger_forest;
}

CirFrozenForest *Program::ensure_source_forest()
{
    if ( source_forest_tried )
	return source_forest;
    source_forest_tried = true;
    if ( bind_forest )
    {
	source_forest = bind_forest;
	return source_forest;
    }

    // Raw source ignores producer std/-D/posix config because these bytes are
    // re-tokenized under THIS Program's live policy, but it must match the
    // selected stdlib flavor: libc++ and libstdc++ own different header text.
    // The same carrier walker remains the one owner of discovery/profile order.
    bool cfg_mismatch = false;
    source_forest = probe_forest_chain(forestMatchStdlibFlavor,
				       cfg_mismatch, /*header_only=*/false);
    if ( source_forest )
    {
	source_forest->_work_secs = &_forest_work_seconds;
	source_forest->_work_depth = &_forest_work_depth;
    }
    return source_forest;
}

bool Program::forest_source_path(const std::string &candidate,
				 bool allow_tail, std::string &resolved)
{
    CirFrozenForest *forest = ensure_source_forest();
    if ( !forest )
	return false;
    int unit = forest->find_unit(candidate);
    if ( unit < 0 && allow_tail )
	unit = forest->find_unit_path_tail(candidate);
    if ( unit < 0 || !forest->unit_has_source((uint32_t)unit) )
	return false;
    const char *name = forest->unit_name((uint32_t)unit);
    if ( !name || !*name )
	return false;
    resolved = name;
    return true;
}

bool Program::forest_source_text(const std::string &path, std::string &text)
{
    CirFrozenForest *forest = ensure_source_forest();
    if ( !forest )
	return false;
    int unit = forest->find_unit(path);
    if ( unit < 0 )
	unit = forest->find_unit_path_tail(path);
    if ( unit < 0 || !forest->unit_has_source((uint32_t)unit) )
	return false;
    std::map<uint32_t, std::string>::iterator cached =
	forest_source_cache.find((uint32_t)unit);
    if ( cached == forest_source_cache.end() )
    {
	std::string source_text;
	if ( !forest->unit_source((uint32_t)unit, source_text) )
	    return false;
	cached = forest_source_cache.emplace((uint32_t)unit,
					     std::move(source_text)).first;
    }
    text = cached->second;
    return true;
}

// The discovery chain ended with no usable container: apply the failure
// policy (forest-carriers S3 — the G2 lesson: no SILENT degradation unless
// silent is the configured contract). Called at most once per Program
// (bind_forest_tried gates the caller). strict_require raises through the
// standard Throw path so an embedding host sees it as a compile error.
//
// config_mismatch = a container WAS found but its producer config (std/-D)
// differs from this compile's. Under loud_fallback that is NOT degradation —
// it is the by-design multi-dialect contract (the packed CLI compiling a C
// file against its C++-parsed corpus is the everyday case; a notice there
// would fire on every such compile — caught by the expect_quiet suite tests
// carrying --std= fixtures). strict_require still hard-errors on it: a host
// that REQUIRES frozen state wants to know its compile config cannot bind
// the container it shipped.
void Program::forest_missing_fallback(bool config_mismatch)
{
    switch ( registration_policy.forest_missing_policy )
    {
    case RegistrationPolicy::ForestPolicy::silent_fallback:
	DBG(std::cout << "forest-bind: no container — live parse"
	    << std::endl);
	break;
    case RegistrationPolicy::ForestPolicy::loud_fallback:
	if ( config_mismatch )
	{
	    DBG(std::cout << "forest-bind: container config mismatch — live"
		" parse (multi-dialect fall-through, no notice)" << std::endl);
	    break;
	}
	(error_stream ? *error_stream : std::cerr)
	    << "madc: no frozen forest found (probed: " << forest_probed_arms()
	    << "); falling back to live parse — startup will be slower. Point"
	       " --forest-bind=<file> at a container, or silence this with"
	       " --no-forest-bind."
	    << std::endl;
	break;
    case RegistrationPolicy::ForestPolicy::strict_require:
	if ( config_mismatch )
	    Throw(NULL) << "frozen forest required (strict forest policy):"
			   " a container was found but its producer config"
			   " (std/-D defines) does not match this compile"
			<< std::flush;
	Throw(NULL) << "frozen forest required (strict forest policy) but no"
		       " usable container was found (probed: "
		    << forest_probed_arms() << ")" << std::flush;
	break;
    }
}

// --show-stats (R0): flatten the forest-bind counters (Program-side timers +
// the forest/reader-owned decode counters) into the plain struct the stats
// display reads — no CirFrozenForest type leaks to the caller.
Program::ForestBindStats Program::forest_bind_stats() const
{
    ForestBindStats s;
    s.map_secs     = _forest_map_seconds;
    s.open_secs    = _forest_open_seconds;
    s.bind_secs    = _forest_bind_seconds;
    s.restore_secs = _forest_restore_seconds;
    s.declidx_secs = _forest_declidx_seconds;
    s.units_bound  = forest_chain.size();
    s.funcs_eager = _forest_funcs_eager;
    s.funcs_deferred = _forest_funcs_deferred;
    s.funcs_activated = _forest_funcs_activated;
    s.funcs_remaining = forest_deferred_funcs.size();
    if ( bind_forest )
    {
	s.opened         = true;
	s.units_total    = bind_forest->unit_count();
	s.unitload_secs  = bind_forest->_stat_unitload_secs;
	s.unitload_count = bind_forest->_stat_unitload_count;
	s.mat_secs       = bind_forest->_stat_mat_secs;
	s.zstd_frames    = bind_forest->stat_zstd_frames();
	s.zstd_bytes     = bind_forest->stat_zstd_bytes_out();
	s.zstd_secs      = bind_forest->stat_zstd_secs();
	s.copy_calls     = bind_forest->stat_copy_calls();
	s.copy_bytes     = bind_forest->stat_copy_bytes();
    }
    return s;
}

// Is a __need request macro live right now? — the protocol by which a
// C-library header asks the resource-dir stddef.h/stdarg.h for ONE
// definition via re-inclusion (glibc: `#define __need_wchar_t` +
// `#include <stddef.h>`). A live request marks the next include as a
// protocol visit (see the include site): it must re-tokenize the
// protocol-aware text, never a one-shot cache. The list is exactly the
// requests madc's OWN embedded protocol headers implement
// (include/madc/stddef.h's __need arms + stdarg's __va_list) and grows in
// lockstep with them — only embedded-served names need the predicate;
// filesystem headers are never name-deduped or PCH-served, so glibc's
// other __need pairs behave as they always did.
bool Program::need_protocol_macro_live()
{
    static const char *const macros[] = {
	"__need_size_t", "__need_ptrdiff_t", "__need_wchar_t",
	"__need_NULL", "__need_wint_t", "__need___va_list",
    };
    for ( size_t i = 0; i < sizeof(macros) / sizeof(macros[0]); ++i )
	if ( define_map.count(macros[i]) )
	    return true;
    return false;
}

// Map a system include spelling to a frozen grove unit index (-1 = miss). Tries
// the bare spelling first (compiler-builtin/embedded units name themselves, e.g.
// "stddef.h"), then the resolved filesystem path (real headers name their full
// path) — the same resolution the live path would use, so a hit binds the exact
// grove the pack froze.  A compilerless consumer cannot stat() the producer's
// paths, but it still carries their ordered search table: consult those exact
// frozen provider names before falling back to an ambiguous path-tail match.
int Program::forest_unit_for_include(const std::string &incfile)
{
    CirFrozenForest *f = ensure_bind_forest();
    if ( !f )
	return -1;
    int u = f->find_unit(incfile);
    if ( u >= 0 )
	return u;
    std::string fp = resolve_include_path(incfile, /*is_system=*/true);
    if ( !fp.empty() && fp != incfile )
	u = f->find_unit(fp);
    if ( u >= 0 )
	return u;
    // Preserve the producer toolchain's include precedence even when none of
    // its paths exists on the run-only machine.  include_next_search_list is
    // the one builder for the ordered -I + selected-stdlib search list; its
    // stored spellings are also the exact names used by filesystem-frozen
    // units.  This matters when both a C++ wrapper and its C provider share a
    // basename (libstdc++ <math.h> before mingw's native <math.h>).
    std::vector<std::string> search;
    include_next_search_list(search);
    for ( size_t i = 0; i < search.size(); ++i )
    {
	std::string candidate = search[i];
	if ( !candidate.empty() && candidate.back() != '/' )
	    candidate += '/';
	candidate += incfile;
	u = f->find_unit(candidate);
	if ( u >= 0 )
	    return u;
    }
    // Machine-portable arm: filesystem-frozen units carry the PRODUCER's
    // full header path, which this machine's resolver may not be able to
    // spell at all (the hosted darwin groves are frozen from the build
    // container's libc++ tree; run-only Macs have no headers to resolve
    // against). The include SPELLING matched against the unit name's tail
    // components is the identity that travels.
    u = f->find_unit_path_tail(incfile);
    // posix/<name> is an internal storage key, never an alternate tail match
    // for an ordinary native header. Whole providers map to it explicitly at
    // the include site after proving that the native provider is absent.
    if ( u >= 0 )
    {
	const char *matched = f->unit_name((uint32_t)u);
	if ( matched && is_posix_compat_header_name(matched)
	  && !is_posix_compat_header_name(incfile) )
	    u = -1;
	else
	    return u;
    }
    // A filesystem PCH freezes under its .madh provider identity, while the
    // source include still names <header>. A headerless consumer cannot probe
    // that file, so the portable tail mapping must try the PCH spelling too.
    u = f->find_unit_path_tail(incfile + ".madh");
    if ( u >= 0 )
    {
	const char *matched = f->unit_name((uint32_t)u);
	if ( matched && is_posix_compat_header_name(matched) )
	    return -1;
    }
    return u;
}

// v40 bind eligibility (task #57): every unit in the root's closure must
// find its frozen EXTERNAL branch dependencies reproduced by the consumer's
// live macro tables. A mismatch means the frozen unit's PP conditionals took
// different arms than a live parse here would — binding it silently loses
// (or gains) declarations: mingw stdlib.h defined errno before errno.h
// parsed in the pack TU, so errno.h froze with its own errno define SKIPPED;
// a consumer including <errno.h> alone must live-parse it instead. Units
// already bound by an earlier include pass trivially — their state IS the
// live state their own check ran against.
// Does this unit's own frozen PP-event stream (re)define `nm`? The
// include-once prune's test: an undefined-at-freeze dep that is defined HERE
// and that the unit itself defines is the unit's own GUARD — the consumer
// already holds this unit's content (an earlier declined root live-parsed
// it), so the unit prunes as satisfied exactly like a live re-include.
bool Program::forest_unit_defines_macro(uint32_t unit, const char *nm)
{
    std::vector<uint32_t> ev;
    if ( !bind_forest->unit_pp_events(unit, ev) )
	return false;
    for ( size_t k = 0; k + 5 <= ev.size(); )
    {
	uint32_t name_id = ev[k], tag_flags = ev[k + 1], nparams = ev[k + 4];
	uint8_t tag = (uint8_t)(tag_flags & 0xff);
	const char *enm = bind_forest->pool_str(name_id);
	if ( enm && tag != PackMacroEvent::peUndef && strcmp(enm, nm) == 0 )
	    return true;
	k += 5 + (tag == PackMacroEvent::peDefineFn ? nparams : 0);
    }
    return false;
}

// A unit-granular live include is safe only for a standalone HEADER husk, not
// for a contextual fragment such as glibc bits/libc-header-start.h or
// bits/mathcalls.h. The format already carries both facts needed to tell them
// apart without names or thresholds: a husk has no frozen declaration surface,
// and a standalone header consulted an undefined-at-entry macro that its own PP
// stream defines (its include guard). Context-only fragments have no such guard;
// non-husks have a non-empty declaration index and stay on the broad rollout
// path even when one conditional branch was absent at freeze.
bool Program::forest_unit_is_standalone_husk(
	uint32_t unit, const std::vector<uint32_t> &deps)
{
    std::vector<cir_forest_decl_entry> decls;
    if ( !bind_forest->unit_decl_index(unit, decls) || !decls.empty() )
	return false;
    for ( size_t k = 0; k + 4 <= deps.size(); k += 4 )
    {
	if ( deps[k + 1] & 1u )
	    continue;
	const char *nm = bind_forest->pool_str(deps[k]);
	if ( nm && forest_unit_defines_macro(unit, nm) )
	    return true;
    }
    return false;
}

// Pass 1: collect the root's closure (units a bind would install — the
// already-bound are excluded exactly as forest_bind_include excludes them).
void Program::forest_env_collect(uint32_t unit, std::set<uint32_t> &closure)
{
    if ( forest_chain_set.count(unit) || forest_live_present.count(unit)
      || forest_husk_live_pending.count(unit)
      || !closure.insert(unit).second )
	return;
    std::vector<uint32_t> edges;
    if ( bind_forest->unit_edges(unit, edges) )
	for ( size_t i = 0; i < edges.size(); ++i )
	    forest_env_collect(edges[i], closure);
}

bool Program::forest_bind_env_ok(uint32_t unit, std::set<uint32_t> &visited)
{
    (void)visited;
    return forest_bind_env_ok(unit);
}

bool Program::forest_bind_env_ok(uint32_t root)
{
    // v40 rollout knob (task #57): the mixed bind/live DECL frontier the
    // decline path exposes is now absorbed (denotes_same_type tolerance +
    // typedef/class live-wins), and the mismatch handling below is the
    // three-way prune/inert-bind/decline redesign. The check still ships
    // default-OFF until the knob-on packed-suite burndown reaches 0 and the
    // #25 packed-lane latency check passes (declines add live parsing).
    // MADC_FOREST_ENV_CHECK=1 turns the BROAD checks on, and the Linux
    // forest_bind_gate runs its whole battery WITH them on, so the parked
    // mechanism cannot rot. Missing-content units are different: a frozen
    // want-defined/live-undefined branch cannot be supplied by any bind, so
    // their exact-unit live recovery is always enabled.
    static int enabled = -1;
    if ( enabled < 0 )
    {
	const char *e = ::getenv("MADC_FOREST_ENV_CHECK");
	enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    std::set<uint32_t> closure;
    std::set<uint32_t> husk_live;
    forest_env_collect(root, closure);
    for ( std::set<uint32_t>::iterator ui = closure.begin();
	  ui != closure.end(); ++ui )
    {
	uint32_t unit = *ui;
	// Mask verdicts are rebuilt on every check of this unit — an earlier
	// root's declined check may have recorded masks against live tables
	// that its own live parse then changed.
	forest_pp_install_mask.erase(unit);
	std::vector<uint32_t> deps;
	if ( !bind_forest->unit_branch_deps(unit, deps) )
	    continue;
	for ( size_t k = 0; k + 4 <= deps.size(); k += 4 )
	{
	    // A dep whose DEFINER is inside this closure is replay-internal:
	    // the bind installs the definer's events too (sys/cdefs.h
	    // consulting _FEATURES_H under a root that carries features.h).
	    uint32_t definer = deps[k + 3];
	    if ( definer != 0xffffffffu && closure.count(definer) )
		continue;
	    const char *nm = bind_forest->pool_str(deps[k]);
	    if ( !nm )
		continue;
	    bool want_defined = (deps[k + 1] & 1u) != 0;
	    bool have_defined = macro_name_defined(nm);
	    if ( want_defined != have_defined )
	    {
		// Missing content is not a rollout choice. The producer saw an
		// EXTERNAL definition and skipped this unit's conditional block;
		// the consumer does not, so the frozen unit cannot supply what a
		// live parse would expose. If the mismatch is below the root,
		// schedule only that unit for the production include tokenizer and
		// keep the root's other units bindable. If it IS the root, ordinary
		// root fall-through already is the exact-unit live parse.
		if ( want_defined && !have_defined
		  && definer != 0xffffffffu
		  && forest_unit_is_standalone_husk(unit, deps) )
		{
		    if ( unit == root )
		    {
			DBG(std::cout << "forest bind: DECLINE root "
			    << (bind_forest->unit_name(root) ? bind_forest->unit_name(root) : "?")
			    << " — root is a missing-content husk (branch dep '"
			    << nm << "' defined at freeze, undefined here); live-tokenizing root"
			    << std::endl);
			return false;
		    }
		    husk_live.insert(unit);
		    DBG(std::cout << "forest bind: unit "
			<< (bind_forest->unit_name(unit) ? bind_forest->unit_name(unit) : "?")
			<< " is a missing-content husk (branch dep '" << nm
			<< "' defined at freeze, undefined here); root "
			<< (bind_forest->unit_name(root) ? bind_forest->unit_name(root) : "?")
			<< " remains bindable" << std::endl);
		    goto next_unit;	// every frozen dep of this live unit is moot
		}
		// Knob OFF preserves the shipping bind behavior for every broad
		// mismatch. Only the missing-content case above bypasses it.
		if ( !enabled )
		    continue;
		// A want-undefined dep the unit ITSELF defines is the unit's
		// own GUARDED FALLBACK (task #57 prune redesign). Two
		// dispositions, decline is not one of them:
		// 1. PRUNE — only with honest include-once evidence: the
		//    unit's FILE was live-tokenized here (an earlier
		//    declined root live-parsed this header), so its whole
		//    content is already present; skip it like a live
		//    re-include. forest_live_present keeps
		//    forest_bind_include from installing its PP events —
		//    but stays OUT of forest_chain_set, whose membership
		//    the decl-restore filter reads: restoring this unit's
		//    decls BESIDE the live-parsed copy double-defines them
		//    (struct _iobuf).
		// 2. MASKED-BIND — no whole-file evidence: a shared
		//    SUB-BLOCK guard (mingw stdio.h's _FILE_DEFINED live
		//    from wchar.h's copy; stddef NULL; glibc wchar.h's
		//    __attr_dealloc_fclose fallback). The dep record proves
		//    a live-order parse of this unit would take the DEFINED
		//    arm and skip the unit's own define, so the bind
		//    installs everything EXCEPT this macro's define events
		//    (forest_pp_install_mask) — live-true PP state, no
		//    clobber of the live value. The unit's OTHER content —
		//    which is NOT live — installs; decl twins at the shared
		//    block are the mixed seam the typedef/class live-wins +
		//    denotes_same_type tolerance absorb. Pruning here LOSES
		//    that other content; declining cascades libc++ roots
		//    into live parses (the statmem frontier — both were
		//    tried, both broke it).
		// The remaining broad mismatches DECLINE: non-self-defined
		// external configuration consults whose state genuinely
		// changed. Want-defined/have-undefined missing content was
		// isolated to an exact live unit above; extra content binds and
		// is arbitrated through the prune/masked-bind split here.
		if ( !want_defined && have_defined
		  && forest_unit_defines_macro(unit, nm) )
		{
		    if ( forest_unit_file_live_tokenized(unit) )
		    {
			DBG(std::cout << "forest bind: unit "
			    << (bind_forest->unit_name(unit) ? bind_forest->unit_name(unit) : "?")
			    << " already present ('" << nm
			    << "' guard live) — pruned" << std::endl);
			forest_live_present.insert(unit);
			goto next_unit;	// a pruned unit's other deps are moot
		    }
		    forest_pp_install_mask[unit].insert(nm);
		    DBG(std::cout << "forest bind: unit "
			<< (bind_forest->unit_name(unit) ? bind_forest->unit_name(unit) : "?")
			<< " dep '" << nm
			<< "' self-defined guard, live arm wins — binds with define masked"
			<< std::endl);
		    continue;	// this dep only; the unit's other deps still gate
		}
		DBG(std::cout << "forest bind: DECLINE root "
		    << (bind_forest->unit_name(root) ? bind_forest->unit_name(root) : "?")
		    << " — unit "
		    << (bind_forest->unit_name(unit) ? bind_forest->unit_name(unit) : "?")
		    << " branch dep '" << nm << "' "
		    << (want_defined ? "defined" : "undefined")
		    << " at freeze, opposite here" << std::endl);
		return false;
	    }
	    if ( enabled && want_defined && (deps[k + 1] & 2u) )
	    {
		const char *v = deps[k + 2] ? bind_forest->pool_str(deps[k + 2]) : NULL;
		std::string have;
		if ( std::string *dv = define_map.find(nm) )
		    have = *dv;
		else if ( MacroDef *mv = macro_map.find(nm) )
		    have = mv->body;
		if ( have != std::string(v ? v : "") )
		{
		    DBG(std::cout << "forest bind: DECLINE root "
			<< (bind_forest->unit_name(root) ? bind_forest->unit_name(root) : "?")
			<< " — unit "
			<< (bind_forest->unit_name(unit) ? bind_forest->unit_name(unit) : "?")
			<< " branch dep '" << nm << "' value differs" << std::endl);
		    return false;
		}
	    }
	}
	next_unit:;
    }
    forest_husk_live_pending.insert(husk_live.begin(), husk_live.end());
    return true;
}

// Bind a grove unit and its include closure: post-order DFS over the unit's
// frozen edges so an includee's PP delta installs before the includer's own
// (matching the common "includes at top, defines after" header shape). The
// forest_chain_set done-marker prunes a unit bound by an earlier #include;
// forest_bind_walking breaks include cycles. After the walk every reachable
// unit's macros are live and it is recorded in forest_chain. The decl records
// (symbol tables) restore ONCE per compile at the call site — never re-parse.
void Program::forest_bind_include(uint32_t unit)
{
    if ( forest_chain_set.count(unit) || forest_bind_walking.count(unit)
      || forest_live_present.count(unit) )
    {
	return;
    }
	const char *unit_name = bind_forest->unit_name(unit);
	if ( unit_name && is_posix_compat_header_name(unit_name) )
	{
		const std::string base = std::string(unit_name).substr(
			sizeof("posix/") - 1);
		if ( !is_posix_compat_header_allowed(base) )
			return;
	}
    // A missing-content child is the one unit a bind cannot reconstruct: its
    // producer-side external macro hid declarations that are live here. Feed
    // its portable header spelling through the ONE production include path;
    // that path owns search order, disk/embedded selection, preprocessing,
    // include guards, and token provenance. Keep it out of forest_chain_set so
    // forest_restore_decls cannot restore the husk's incomplete frozen surface.
    if ( forest_husk_live_pending.count(unit) )
    {
	if ( !unit_name || !*unit_name )
	    Throw << "Frozen missing-content unit has no source identity" << flush;
	std::string live_header(unit_name);
	const std::string madh_suffix = ".madh";
	if ( live_header.size() > madh_suffix.size()
	  && live_header.compare(live_header.size() - madh_suffix.size(),
				 madh_suffix.size(), madh_suffix) == 0 )
	    live_header.erase(live_header.size() - madh_suffix.size());
	// Re-include the EXACT unit by its canonical path, never its basename.
	// A basename is ambiguous across the corpus (<stat.h> names glibc's
	// bits/stat.h, linux/stat.h AND sys/stat.h) and resolves by grove
	// lookup order — a corpus reshuffle flipped bits/stat.h's fallback
	// onto linux/stat.h, silently dropping __S_IFMT for every headerless
	// consumer of <sys/stat.h>. An absolute spelling passes through
	// resolve_include_path verbatim, and read_resolved_include serves it
	// from disk or the pack's raw-source slot. An embedded unit's name
	// has no directory and is already the include spelling.
	if ( live_header.empty() )
	    Throw << "Frozen missing-content unit has no live include spelling: "
		  << unit_name << flush;
	DBG(std::cout << "forest bind: missing-content husk " << unit_name
	    << " — live-tokenizing <" << live_header << "> only" << std::endl);
	forest_bind_walking.insert(unit);
	try
	{
	    tokenize_synthetic_system_include(live_header, unit_name);
	}
	catch (...)
	{
	    forest_bind_walking.erase(unit);
	    throw;
	}
	forest_bind_walking.erase(unit);
	forest_husk_live_pending.erase(unit);
	forest_live_present.insert(unit);
	return;
    }
    ForestWorkFrame _fw(&_forest_work_seconds, &_forest_work_depth);
    forest_bind_walking.insert(unit);
    // --show-stats (R0): per-unit SELF cost — edge decode + PP install,
    // recursion excluded — so the accumulated total sums cleanly.
    double _t0 = forest_stat_now();
    std::vector<uint32_t> edges;
    bool have_edges = bind_forest->unit_edges(unit, edges);
    double _self = forest_stat_now() - _t0;
    // Children before this unit's own PP delta is right while children are
    // merely BOUND — a bind evaluates no #if, so install order cannot change
    // what it sees. A missing-content husk child is different: it is LIVE-
    // PARSED (see forest_husk_live_pending above), and a live parse DOES
    // evaluate #if, against whatever the macro tables hold at that moment.
    // With this unit's exports still uninstalled, an internal header gets
    // parsed as if its public front had never run — glibc's bits/stat.h then
    // hits its own `#if !defined _SYS_STAT_H && !defined _FCNTL_H` guard and
    // #errors, because sys/stat.h had bound but not yet defined _SYS_STAT_H.
    //
    // So when any child is a pending husk, this unit's PP lands FIRST. That is
    // an APPROXIMATION of live order, not a reproduction of it: the event
    // stream carries no position for the child's include site, so the whole
    // export set installs rather than the prefix that preceded the child. It
    // is strictly closer to a live parse than installing nothing, and the
    // headers this affects define their guard before including their internals
    // — which is the whole reason the guard exists.
    bool husk_child = false;
    if ( have_edges )
	for ( size_t i = 0; i < edges.size() && !husk_child; ++i )
	    husk_child = forest_husk_live_pending.count(edges[i]) != 0;
    if ( husk_child )
	forest_install_pp(unit);
    if ( have_edges )
	for ( size_t i = 0; i < edges.size(); ++i )
	    forest_bind_include(edges[i]);
    double _t1 = forest_stat_now();
    if ( !husk_child )
	forest_install_pp(unit);
    forest_chain.push_back(unit);
    forest_chain_set.insert(unit);
    forest_bind_walking.erase(unit);
    _self += forest_stat_now() - _t1;
    _forest_bind_seconds += _self;
    const char *nm = bind_forest->unit_name(unit);
    _forest_unit_bind_costs.push_back(
	std::make_pair(std::string(nm ? nm : "?"), _self));
}

// Apply one unit's frozen PP-export delta to the live macro tables, replaying
// the exact define_map / macro_map writes the lexer's #define/#undef handlers
// perform. The event stream is flat u32:
//   [name_id, tag_flags, body_id, variadic_param_id, nparams, <nparams ids>]...
// with the low byte of tag_flags the PackMacroEvent tag and bit 8 the variadic
// flag. Pool ids resolve through the container's own string pool.
void Program::forest_install_pp(uint32_t unit)
{
    std::vector<uint32_t> ev;
    if ( !bind_forest->unit_pp_events(unit, ev) )
	return;
    // Masked-bind (task #57): a define event for a macro forest_bind_env_ok
    // masked on this unit is the unit's own guarded fallback whose guard is
    // live-defined — a live-order parse would skip it, so the install does
    // too (the live value survives; undef events still apply).
    std::map<uint32_t, std::set<std::string> >::const_iterator mask_it =
	forest_pp_install_mask.find(unit);
    const std::set<std::string> *mask =
	mask_it == forest_pp_install_mask.end() ? NULL : &mask_it->second;
    for ( size_t k = 0; k + 5 <= ev.size(); )
    {
	uint32_t name_id = ev[k], tag_flags = ev[k + 1], body_id = ev[k + 2];
	uint32_t vpar_id = ev[k + 3], nparams = ev[k + 4];
	const char *nm = bind_forest->pool_str(name_id);
	uint8_t tag = (uint8_t)(tag_flags & 0xff);
	if ( nm && mask && tag != PackMacroEvent::peUndef && mask->count(nm) )
	{
	    DBG(std::cout << "forest bind: unit "
		<< (bind_forest->unit_name(unit) ? bind_forest->unit_name(unit) : "?")
		<< " define '" << nm << "' masked at install (live guard wins)"
		<< std::endl);
	    k += 5 + (tag == PackMacroEvent::peDefineFn ? nparams : 0);
	    continue;
	}
	if ( nm )
	{
	    std::string name(nm);
	    if ( tag == PackMacroEvent::peUndef )
	    {
		define_map.erase(name);
		macro_map.erase(name);
	    }
	    else if ( tag == PackMacroEvent::peDefine )
	    {
		const char *b = body_id ? bind_forest->pool_str(body_id) : NULL;
		define_map[name] = b ? std::string(b) : std::string();
		note_std_abi_define(name, define_map[name]);
	    }
	    else // peDefineFn
	    {
		MacroDef m;
		for ( uint32_t p = 0; p < nparams; ++p )
		    if ( const char *pn = bind_forest->pool_str(ev[k + 5 + p]) )
			m.params.push_back(pn);
		if ( tag_flags & CIR_FOREST_PP_VARIADIC )
		    m.variadic = true;
		if ( vpar_id )
		    if ( const char *vp = bind_forest->pool_str(vpar_id) )
			m.variadic_param = vp;
		if ( const char *b = body_id ? bind_forest->pool_str(body_id) : NULL )
		    m.body = b;
		macro_map[name] = m;
	    }
	}
	k += 5 + nparams;
    }
}

// --- the generated include tables, read through ONE pair of accessors --------
//
// System include paths are captured at BUILD time from the configured compiler's
// own search list (scripts/gen_sys_includes.sh -> src/sys_include_paths.cpp), so
// madc searches the SAME dirs the toolchain does — including the C++ paths the
// old hardcoded C-only list lacked. There is one list per C++ standard library
// flavor; `-stdlib=` picks which, defaulting to the flavor $(CXX) uses.
//
// Selecting a flavor REPLACES the search list rather than reordering it, which
// is what clang's driver does and what correctness requires: libc++'s <cstdlib>
// reaches the C library through `#include_next <stdlib.h>`, so leaving the GNU
// C++ dirs on the path behind libc++ makes that walk land in
// /usr/include/c++/NN/stdlib.h and fail on its `using std::abort;`.
// Canonicalize a filesystem path for COMPARISON. The one owner of that step:
// include bookkeeping keys files by their resolved path, and the search-dir
// prefixes must be resolved the same way or the two never match (clang reports
// `.../bin/../include/c++/v1`). Returns the input unchanged when it does not
// resolve — a cross/hosted table may name a sysroot absent from this machine.
// NOT for resolving argv[0] or a dladdr image name; those are different rules
// with their own call sites.
static std::string canonical_path_for_compare(const std::string &path)
{
    std::string out = madc::detail::resolve_real_path(path.c_str());
    return out.empty() ? path : out;
}

static const char *madc_fallback_include_paths[] = {
    "/usr/local/include/",
    "/usr/include/",
    "/usr/include/x86_64-linux-gnu/",
    (const char *)0
};

const char *const *Program::sys_include_paths() const
{
    const madc_stdlib_flavor *f = active_stdlib_flavor();
    // Minimal C list when detection produced nothing (no compiler at build time).
    if ( !f->paths || !f->paths[0] )
	return madc_fallback_include_paths;
    if ( registration_policy.enable_sysroot_includes )
	return f->paths;
    // The toolchain-only view (registration_policy.enable_sysroot_includes ==
    // false): the compiler reports its search list in a fixed order — the
    // C++ stdlib dirs, then its own builtin (compiler-owned) dir, then the
    // sysroot's C dirs and frameworks — so "everything up to and including the
    // compiler-owned dir" IS the set of roots the toolchain owns, and what
    // follows is the host's C library / SDK. Cached per flavor, NULL-terminated
    // like the table it views. A table with no compiler-owned slot cannot be
    // cut honestly: refuse loudly rather than guess a position.
    if ( _toolchain_paths_flavor == f && !_toolchain_paths.empty() )
	return _toolchain_paths.data();
    const char *owned = f->compiler_owned_dir ? f->compiler_owned_dir : "";
    _toolchain_paths.clear();
    bool cut = false;
    for ( int i = 0; f->paths[i]; ++i )
    {
	_toolchain_paths.push_back(f->paths[i]);
	if ( *owned && strcmp(f->paths[i], owned) == 0 )
	{
	    cut = true;
	    break;
	}
    }
    if ( !cut )
    {
	_toolchain_paths.clear();
	throw std::runtime_error("--no-sysroot-includes: this build's system include table has no "
				 "compiler-owned dir to cut at (flavor '" + std::string(f->name ? f->name : "")
				 + "'); cannot separate toolchain roots from sysroot roots");
    }
    _toolchain_paths.push_back((const char *)0);
    _toolchain_paths_flavor = f;
    return _toolchain_paths.data();
}

bool Program::posix_compat_enabled() const
{
	// The TARGET owns this, not the host (datadef.h: "never re-test _WIN32 at
	// a consumer"). target_windows() is the third member of the target-property
	// family beside target_llp64() / target_microsoft_bitfields(), so a future
	// --target= knob reaches this predicate the same way it reaches the widths.
	return target_windows() && registration_policy.enable_posix_compat;
}

bool Program::is_posix_compat_header_name(const std::string &name) const
{
	return name.compare(0, sizeof("posix/") - 1, "posix/") == 0;
}

bool Program::is_posix_compat_header_allowed(const std::string &name) const
{
	if ( !posix_compat_enabled() )
		return false;
	if ( !registration_policy.restrict_headers_to_allowlist
	  && registration_policy.allowed_headers.empty() )
		return true;
	for ( size_t i = 0; i < registration_policy.allowed_headers.size(); ++i )
		if ( registration_policy.allowed_headers[i] == name )
			return true;
	return false;
}

void Program::tokenize_posix_header_supplement(const std::string &incfile)
{
	if ( !is_posix_compat_header_allowed(incfile) )
		return;
	const std::string supplement = std::string("posix/") + incfile;
	const std::string *embedded = find_embedded_header(supplement);
	if ( !embedded )
		return;
	// A grove stores the real header and its supplement as ordered sibling
	// edges. An explicit bind of the real header therefore still owes the
	// supplement; bind that existing unit too so a later parent bind cannot
	// restore the supplement's declarations a second time. Forests stamped
	// before this policy bit are rejected by the config-word check.
	if ( registration_policy.enable_forest_bind )
	{
		CirFrozenForest *forest = ensure_bind_forest();
		int fu = forest ? forest->find_unit(supplement) : -1;
		if ( fu >= 0 && forest_bind_env_ok((uint32_t)fu) )
		{
			forest_bind_include((uint32_t)fu);
			mark_embedded_include_flag(supplement);
			return;
		}
	}
	// This is compiler-owned delta text, not another ordinary include lookup:
	// no -I/PCH provider may outrank it. Serving from the restored includer
	// records the real header and its supplement as ordered sibling edges.
	tokenize_embedded_header_text(supplement, *embedded, false);
}

// "Does this path name a readable file" — one owner, defined further down with
// the PCH candidate walk that is its other caller.
bool madc_lexer_file_exists(const std::string &path);

// <dlfcn.h> is the case the SUPPLEMENT model cannot serve: mingw-w64 ships no
// such header at all, so there is no real provider to augment and nothing to
// shadow. posix/<name> IS the provider. Gated on the walk's ACTUAL outcome —
// the resolved path names no readable file — so §8's "serve only where the
// native toolchain lacks it" is decided by evidence, not prediction. On a
// POSIX host the native header always resolves, so this never fires there.
// The DECISION above is a predicate of its own because TWO consumers need it:
// the serve arm below acts on it, and __has_include answers with it. They sit
// at the same position in the resolution order — after the filesystem walk —
// so "can I include this?" and "will including it work?" cannot disagree. A
// file-open probe alone cannot see a provider that is on no disk, and it
// answered NO for <dlfcn.h> while the include below served it: the canonical
// `#if __has_include(<dlfcn.h>)` idiom then took the no-dlfcn branch on a
// target that has it. `text`, when given, receives the embedded provider so
// the caller that serves does not repeat the lookup.
bool Program::posix_whole_provider_serves(const std::string &incfile,
					  const std::string &resolved,
					  const std::string **text)
{
	if ( !is_posix_compat_header_allowed(incfile) )
		return false;
	if ( resolved_include_provider_exists(resolved) )
		return false;
	const std::string *embedded =
		find_embedded_header(std::string("posix/") + incfile);
	if ( !embedded )
		return false;
	if ( text )
		*text = embedded;
	return true;
}

// Serve it: the predicate decided, this acts. Prefer the frozen forest unit
// when forest-bind is on, else tokenize the embedded text.
bool Program::tokenize_posix_whole_provider(const std::string &incfile,
					   const std::string &resolved)
{
	const std::string *embedded = NULL;
	if ( !posix_whole_provider_serves(incfile, resolved, &embedded) )
		return false;
	const std::string provider = std::string("posix/") + incfile;
	if ( registration_policy.enable_forest_bind )
	{
	    CirFrozenForest *forest = ensure_bind_forest();
	    int fu = forest ? forest->find_unit(provider) : -1;
	    if ( fu >= 0 && forest_bind_env_ok((uint32_t)fu) )
	    {
		forest_bind_include((uint32_t)fu);
		mark_embedded_include_flag(provider);
		return true;
	    }
	}
	tokenize_embedded_header_text(provider, *embedded, false);
	return true;
}

const char *Program::compiler_owned_include_dir() const
{
    const madc_stdlib_flavor *f = active_stdlib_flavor();
    return f->compiler_owned_dir ? f->compiler_owned_dir : "";
}

// The same search dirs, resolved through realpath — because a compiler reports
// its search list in whatever spelling it computed, and those spellings are not
// always canonical. clang says `/usr/lib/llvm-18/bin/../include/c++/v1`, and a
// file found there is recorded by its realpath, so a raw prefix compare between
// the two MISSES and every libc++ header classifies as user code. Cached per
// flavor: realpath touches the filesystem, and this is asked once per token file.
//
// Entries that do not resolve (a cross/hosted table naming a sysroot that does
// not exist on this machine) keep their original spelling, so the raw compare
// below still covers them.
const std::vector<std::string> &Program::sys_include_prefixes_canonical() const
{
    const madc_stdlib_flavor *f = active_stdlib_flavor();
    if ( _canon_prefix_flavor == f && !_canon_prefixes.empty() )
	return _canon_prefixes;
    _canon_prefixes.clear();
    const char *const *paths = sys_include_paths();
    for ( int i = 0; paths[i]; ++i )
    {
	std::string p = canonical_path_for_compare(paths[i]);
	if ( p != paths[i] && (p.empty() || p.back() != '/') )
	    p += '/';		// realpath drops the trailing '/' a prefix needs
	_canon_prefixes.push_back(p);
    }
    _canon_prefix_flavor = f;
    return _canon_prefixes;
}

std::string Program::stdlib_flavor_names() const
{
    std::string out;
    for ( int i = 0; madc_stdlib_flavors[i].name; ++i )
    {
	if ( !*madc_stdlib_flavors[i].name )
	    continue;
	if ( !out.empty() )
	    out += ", ";
	out += madc_stdlib_flavors[i].name;
    }
    // A build host with no C++ compiler to probe records no flavor NAME at all;
    // say so rather than printing an empty list.
    return out.empty() ? std::string("none — no C++ compiler was probed at build time")
		       : out;
}

const madc_stdlib_flavor *madc_stdlib_flavor_lookup(const std::string &name)
{
    if ( name.empty() )
	return NULL;	// an empty name must not match an unnamed flavor entry
    for ( int i = 0; madc_stdlib_flavors[i].name; ++i )
	if ( name == madc_stdlib_flavors[i].name )
	    return &madc_stdlib_flavors[i];
    return NULL;	// unknown or not built with this flavor
}

bool Program::set_stdlib_flavor_option(const std::string &arg)
{
    static const std::string opt = "-stdlib=";
    if ( arg.compare(0, opt.size(), opt) != 0 )
	return false;
    const madc_stdlib_flavor *f = madc_stdlib_flavor_lookup(arg.substr(opt.size()));
    if ( !f )
	return false;	// caller diagnoses with stdlib_flavor_names()
    stdlib_flavor = f;
    return true;
}

const madc_stdlib_flavor *Program::active_stdlib_flavor() const
{
    return stdlib_flavor ? stdlib_flavor : &madc_stdlib_flavors[0];
}

// The std:: inline ABI namespace follows the PARSED stdlib configuration —
// never a literal keyed on the flavor name. Each stdlib declares it via the
// macro that exists for exactly this purpose:
//   libc++:    _LIBCPP_ABI_NAMESPACE  (the namespace itself, e.g. __1)
//   libstdc++: _GLIBCXX_USE_CXX11_ABI (1 => the cxx11-tagged ABI, 0 => not)
// Pushes the fact into the mangler the moment it is recorded, from every
// define_map write site (#define directive, forest PP replay, CLI -D).
void Program::note_std_abi_define(const std::string &name, const std::string &value)
{
    if ( name == "_LIBCPP_ABI_NAMESPACE" )
    {
	size_t b = value.find_first_not_of(" \t");
	size_t e = value.find_last_not_of(" \t");
	if ( b == std::string::npos )
	    return;		// empty value: nothing to record
	std::string ns = value.substr(b, e - b + 1);
	DBG(std::cout << "std ABI namespace (libc++): " << ns << std::endl);
	madc_mangle_set_stdlib_llvm(ns);
    }
    else if ( name == "_GLIBCXX_USE_CXX11_ABI" )
    {
	bool on = strtol(value.c_str(), NULL, 10) != 0;
	DBG(std::cout << "std ABI namespace (libstdc++): cxx11=" << on << std::endl);
	madc_mangle_set_stdlib_gnu(on);
    }
}

std::string Program::resolve_include_path(const std::string &incfile, bool is_system)
{
    if ( incfile.empty() || incfile[0] == '/' )
	return incfile;

    // Standard C include search order:
    //   #include <file.h>  → embedded headers (checked before this),
    //                        then system paths
    //   #include "file.h"  → current source directory, then -I paths

    if ( is_system )
    {
	// <file.h>: -I paths first (GCC searches -I for both "" and <>),
	// then system include paths, then current source directory as
	// last resort (needed for local header copies in project trees).
	for ( size_t i = 0; i < include_paths.size(); ++i )
	{
	    std::string &dir = include_paths[i];
	    std::string candidate = dir + (dir.empty() || dir.back() == '/' ? "" : "/") + incfile;
	    std::ifstream probe(candidate.c_str());
	    if ( probe.good() )
		return candidate;
	}
	// Then the selected flavor's system include paths (see sys_include_paths()).
	const char *const *sys_paths = sys_include_paths();
	for ( int i = 0; sys_paths[i]; ++i )
	{
	    std::string candidate = std::string(sys_paths[i]) + incfile;
	    std::ifstream probe(candidate.c_str());
	    if ( probe.good() )
		return candidate;
	    std::string frozen_path;
	    if ( forest_source_path(candidate, /*allow_tail=*/false, frozen_path) )
		return frozen_path;
	}
	// Fall back to current source directory — handles local header
	// copies (e.g. libpq-fe.h sitting next to the .c that includes it)
	std::string cur_dir = current_source_directory();
	if ( !cur_dir.empty() )
	{
	    std::string local = cur_dir + incfile;
	    std::ifstream probe(local.c_str());
	    if ( probe.good() )
		return local;
	}
	std::string frozen_path;
	if ( forest_source_path(incfile, /*allow_tail=*/true, frozen_path) )
	    return frozen_path;
	return incfile; // not found — will fail at open
    }

    // "file.h": current source directory, then -I paths — then the WHOLE
    // <...> chain. C11 6.10.2p3: a quoted include whose quoted-specific
    // search fails "is reprocessed as if it read #include <...>", and gcc
    // does exactly that (an installed header's #include "libmadc/options.h"
    // resolves via the /usr/local/include root after the includer-relative
    // candidate misses). Delegating keeps the system chain ONE owner; it
    // also makes the not-found error name the UNDOUBLED spelling instead
    // of includer_dir + incfile.
    std::string cur_dir = current_source_directory();
    if ( !cur_dir.empty() )
    {
	std::string local = cur_dir + incfile;
	std::ifstream probe(local.c_str());
	if ( probe.good() )
	    return local;
    }
    return resolve_include_path(incfile, /*is_system=*/true);
}

// Is this source file one that came from a system/toolchain include directory
// (glibc / libstdc++ / their bits/), as opposed to the user's own .mad/.c/.h?
// Data-driven (project Rule #7): a prefix match against the SAME compiler-derived
// search dirs `#include <...>` uses (sys_include_paths(), the selected stdlib
// flavor's generated table) — never a namespace==std or class-name test.
// Used by the CIR backend to gate emission of library inline-method bodies
// (reachability DCE): madc emits a body only for entities it defines; a function
// parsed from a system header is emitted only when reachable from user code.
// An unknown/empty path classifies as NON-system (treat as user code → emit),
// which keeps the conservative "emit it" default for synthetic/compiler-created
// tokens that carry no source file.
bool Program::is_system_header_path(const char *path) const
{
    if ( !path || !*path )
	return false;
    // Embedded headers carry their bare NAME as the token file (they never
    // touch the filesystem). They are system/library surface by definition —
    // notably the hosted-darwin prelude, whose inline bodies must be DCE'd
    // like any system header's (unreachable darwin inlines would otherwise
    // demand libSystem imports from every program).
    if ( find_embedded_header(path) )
	return true;
    const char *const *sys_paths = sys_include_paths();
    for ( int i = 0; sys_paths[i]; ++i )
    {
	const char *prefix = sys_paths[i];
	size_t plen = strlen(prefix);
	if ( plen && strncmp(path, prefix, plen) == 0 )
	    return true;
    }
    // Then the canonical spellings, for a caller that realpath'd its file (as
    // should_tokenize_include does) against a table entry that is not canonical.
    // Getting this wrong is not cosmetic: a system header misread as user code
    // gets madc's require-once semantics instead of gcc's guard-checked
    // multiple-include, which permanently drops the second visit to a header
    // written to be included twice — libc++'s <stddef.h>/<stdint.h> wrappers are
    // exactly that, and <cstddef> #errors when its second visit never happens.
    const std::vector<std::string> &canon = sys_include_prefixes_canonical();
    for ( size_t i = 0; i < canon.size(); ++i )
    {
	if ( !canon[i].empty() && strncmp(path, canon[i].c_str(), canon[i].size()) == 0 )
	    return true;
    }
    return false;
}

// The ordered #include_next walk: the same dirs <> includes search (-I paths
// first, then the compiler-derived system include paths), with the start
// position AFTER the directory the current file was found in. ONE builder for
// both next-walkers — resolve_include_next_path (which filesystem dir serves
// the name) and embedded_wins_include_next (does the embedded slot come
// first) — so their notion of "after the current file" cannot diverge.
size_t Program::include_next_search_list(std::vector<std::string> &search)
{
    for ( size_t i = 0; i < include_paths.size(); ++i )
	search.push_back(include_paths[i]);
    const char *const *sys_paths = sys_include_paths();
    for ( int i = 0; sys_paths[i]; ++i )
	search.push_back(sys_paths[i]);

    // Normalize-with-trailing-slash compare to locate the current file's dir
    // in the search list; #include_next searches only entries AFTER it.
    std::string cur = current_source_directory();
    if ( !cur.empty() && cur.back() != '/' )
	cur += '/';
    for ( size_t i = 0; i < search.size(); ++i )
    {
	std::string d = search[i];
	if ( !d.empty() && d.back() != '/' )
	    d += '/';
	if ( d == cur )
	    return i + 1;
    }
    return 0;
}

// #include_next <file>: behave like a system #include, but search the
// path list starting AFTER the directory the current file was found in.
// Used by libstdc++/libc++ wrapper headers (cstdlib -> stdlib.h, cmath ->
// math.h …) to reach the "real" header of the same name that sits later in
// the search order. The embedded set is consulted by the caller through
// embedded_wins_include_next (position-aware, below); this resolves the
// filesystem fallback for the non-embedded targets.
std::string Program::resolve_include_next_path(const std::string &incfile)
{
    if ( incfile.empty() || incfile[0] == '/' )
	return incfile;

    std::vector<std::string> search;
    size_t start = include_next_search_list(search);
    for ( size_t i = start; i < search.size(); ++i )
    {
	std::string &dir = search[i];
	std::string candidate = dir + (dir.empty() || dir.back() == '/' ? "" : "/") + incfile;
	std::ifstream probe(candidate.c_str());
	if ( probe.good() )
	    return candidate;
	std::string frozen_path;
	if ( forest_source_path(candidate, /*allow_tail=*/false, frozen_path) )
	    return frozen_path;
    }
	std::string frozen_path;
	if ( forest_source_path(incfile, /*allow_tail=*/true, frozen_path) )
	    return frozen_path;
    return incfile; // not found — will fail at open
}

// Does #include_next <name> resolve to madc's EMBEDDED copy? The embedded
// set occupies the compiler-resource-dir slot of the search order, and
// embedded_header_outranked (parser.cpp) asks the ranking question from the
// TOP of the list — right for a plain include, WRONG for #include_next,
// whose walk starts after the current file's directory: a directory can
// only beat the embedded copy if it sits between that position and the
// slot AND supplies the name. The distinction is what makes the
// hosted-darwin C++ groves work at all: libc++'s C wrappers (c++/v1's
// stdio.h, wchar.h, …) #include_next the C library, and on an Apple target
// the C library IS the embedded prelude — there is nothing on disk after
// the slot. On glibc hosts the embedded set does not carry libc names (the
// retired-shims law), so this answers false and the filesystem walk serves
// /usr/include exactly as before.
bool Program::embedded_wins_include_next(const std::string &incfile)
{
    if ( !find_embedded_header(incfile) || !is_embedded_header_allowed(incfile) )
	return false;
    const std::string owned = compiler_owned_include_dir();
    // No slot recorded (no compiler at build time, or the fallback list is
    // in use): the embedded set keeps its historical unconditional
    // precedence — the same answer embedded_header_outranked gives.
    if ( owned.empty() )
	return true;
    std::vector<std::string> search;
    size_t start = include_next_search_list(search);
    for ( size_t i = start; i < search.size(); ++i )
    {
	if ( search[i] == owned )
	    return true;    // reached the slot before any real provider
	std::string candidate = search[i]
	    + (search[i].empty() || search[i].back() == '/' ? "" : "/") + incfile;
	std::ifstream probe(candidate.c_str());
	if ( probe.good() )
	    return false;   // a real directory between here and the slot wins
    }
    return true;   // slot not reached in the list — preserve the old order
}

// Detect the classic include guard of a header file: the first significant
// line is `#ifndef NAME` (or `#if !defined(NAME)`) and its matching `#endif`
// closes the file with nothing significant after it. Returns the guard macro
// name, or "" when the file is NOT fully guard-wrapped (e.g. glibc's
// bits/mathcalls.h, which is INTENTIONALLY included multiple times with a
// different _Mdouble_ each pass).
// Reads through read_resolved_include(), NOT the filesystem: on a compiler-less
// target the header exists only in the packed forest, and a detector that could
// not see it reported "no guard" — indistinguishable from a genuinely
// guard-less header, which inverts gcc's multiple-include optimization. See the
// reader's comment for the failure this caused.
std::string Program::detect_include_guard(const std::string &file_path)
{
    std::string file_text;
    if ( !read_resolved_include(file_path, file_text) )
	return std::string();
    std::istringstream in(file_text);
    std::string guard;
    int depth = 0;
    bool seen_open = false;     // saw the opening #ifndef
    bool closed = false;        // matching #endif reached (depth back to 0)
    bool in_block_comment = false;
    std::string line;
    while ( std::getline(in, line) )
    {
	// Fold line continuations so a split directive reads whole.
	while ( !line.empty() && line.back() == '\\' && in )
	{
	    std::string cont;
	    if ( !std::getline(in, cont) )
		break;
	    line.pop_back();
	    line += cont;
	}
	// Strip comments for significance testing.
	std::string sig;
	for ( size_t i = 0; i < line.size(); ++i )
	{
	    if ( in_block_comment )
	    {
		if ( line[i] == '*' && i + 1 < line.size() && line[i+1] == '/' )
		{ in_block_comment = false; ++i; }
		continue;
	    }
	    if ( line[i] == '/' && i + 1 < line.size() && line[i+1] == '*' )
	    { in_block_comment = true; ++i; continue; }
	    if ( line[i] == '/' && i + 1 < line.size() && line[i+1] == '/' )
		break;
	    sig += line[i];
	}
	size_t p = sig.find_first_not_of(" \t");
	if ( p == std::string::npos )
	    continue;
	if ( closed )
	    return std::string();   // significant content after the guard's #endif
	if ( sig[p] != '#' )
	{
	    if ( !seen_open )
		return std::string();   // code before any guard
	    continue;
	}
	++p;
	while ( p < sig.size() && (sig[p] == ' ' || sig[p] == '\t') ) ++p;
	std::string dir;
	while ( p < sig.size() && (isalpha((unsigned char)sig[p]) || sig[p] == '_') )
	    dir += sig[p++];
	if ( !seen_open )
	{
	    std::string name;
	    if ( dir == "ifndef" )
	    {
		while ( p < sig.size() && (sig[p] == ' ' || sig[p] == '\t') ) ++p;
		while ( p < sig.size() && (isalnum((unsigned char)sig[p]) || sig[p] == '_') )
		    name += sig[p++];
	    }
	    else if ( dir == "if" )
	    {
		// `#if !defined(NAME)` / `#if !defined NAME` (whole condition)
		std::string rest = sig.substr(p);
		size_t b = rest.find_first_not_of(" \t");
		if ( b != std::string::npos && rest[b] == '!' )
		{
		    size_t d = rest.find("defined", b);
		    if ( d != std::string::npos )
		    {
			d += 7;
			while ( d < rest.size() && (rest[d]==' '||rest[d]=='\t'||rest[d]=='(') ) ++d;
			while ( d < rest.size() && (isalnum((unsigned char)rest[d]) || rest[d]=='_') )
			    name += rest[d++];
			while ( d < rest.size() && (rest[d]==' '||rest[d]=='\t'||rest[d]==')') ) ++d;
			if ( d < rest.size() )
			    name.clear();   // trailing condition — not a pure guard
		    }
		}
	    }
	    if ( name.empty() )
		return std::string();
	    guard = name;
	    seen_open = true;
	    depth = 1;
	    continue;
	}
	if ( dir == "if" || dir == "ifdef" || dir == "ifndef" )
	    ++depth;
	else if ( dir == "endif" )
	{
	    if ( --depth == 0 )
		closed = true;
	}
    }
    return (seen_open && closed) ? guard : std::string();
}

// --- B4a pack-time forest recording hooks (grove payload v2; see
// docs/plans/2026-07-04-forest-default-mode-design.md §2). Every hook is a
// no-op unless pack_recording is on (--freeze / --freeze-append), so default
// lexing pays one predicted branch per site.

const char *Program::pack_current_unit()
{
    if ( !pack_recording )
	return NULL;
    if ( pack_protocol_unit )	// __need serving: every effect (the served
	return pack_protocol_unit;	// #define/#undef) belongs to the includer
    const char *f = source.fname();
    if ( !f || !*f )
	return NULL;
    return intern_file(f);
}

// __need protocol serving window (see the member comment in madc.h). begin()
// captures the includer as the owner — call it BEFORE the source swap, while
// the includer is still the current file — and returns the prior owner so
// nested servings keep the OUTERMOST includer; end() restores it.
const char *Program::pack_protocol_serving_begin()
{
    const char *saved = pack_protocol_unit;
    if ( pack_recording && !pack_protocol_unit )
	pack_protocol_unit = pack_current_unit();
    return saved;
}

void Program::pack_protocol_serving_end(const char *saved)
{
    pack_protocol_unit = saved;
}

void Program::pack_note_unit(const char *interned_file)
{
    if ( !pack_recording || !interned_file )
	return;
    if ( pack_units_seen.insert(interned_file).second )
	pack_unit_order.push_back(interned_file);
}

void Program::pack_record_source(const char *interned_file,
				 const std::string &text)
{
    if ( !pack_recording || !interned_file )
	return;
    pack_note_unit(interned_file);
    // A file can be reached repeatedly through its include guard. Its raw
    // bytes are immutable during one freeze; first-wins keeps the original
    // provider and avoids copying the same header on every visit.
    pack_unit_sources.emplace(interned_file, text);
}

// A header reached again through its include guard is SKIPPED, and the skip
// path records the edge — the reference — without ever reading the file. When
// the very FIRST encounter is such a skip, that mints a unit with no content:
// something the corpus points at but can never serve.
//
// It happens whenever madc predefines a guard so the header can be skipped
// wholesale, which is exactly what it does for <stdc-predef.h> (gcc preincludes
// that header; madc defines _STDC_PREDEF_H instead). A consumer whose --std=
// differs from the producer's declines the grove, live-parses features.h from
// the corpus, walks its unconditional `#include <stdc-predef.h>`, and finds a
// husk. On a compiler-less target there is no disk to fall back to. That was 67
// suite failures, and 46 of the Linux corpus's 241 units were such husks.
//
// So the reference and the content are recorded TOGETHER: an edge without bytes
// is not a smaller corpus, it is a broken one. Freeze-time only (pack_recording
// gates it) and at most one read per unit, so ordinary compiles pay nothing.
void Program::pack_record_skipped_source(const std::string &path)
{
    if ( !pack_recording )
	return;
    const char *interned = intern_file(path);
    if ( !interned || pack_unit_sources.count(interned) )
	return;		// already captured by a real tokenization — first wins
    std::string text;
    if ( read_resolved_include(path, text) )
	pack_record_source(interned, text);
}

void Program::pack_record_define(const std::string &name, const std::string &value)
{
    if ( const char *unit = pack_current_unit() )
    {
	pack_note_unit(unit);
	pack_pp_exports[unit].push_back(PackMacroEvent());
	PackMacroEvent &ev = pack_pp_exports[unit].back();
	ev.name = name;
	ev.tag = PackMacroEvent::peDefine;
	ev.value = value;
	pack_macro_origin[name] = PackMacroOrigin{ unit, true };
    }
}

void Program::pack_record_define_fn(const std::string &name, const MacroDef &m)
{
    if ( const char *unit = pack_current_unit() )
    {
	pack_note_unit(unit);
	pack_pp_exports[unit].push_back(PackMacroEvent());
	PackMacroEvent &ev = pack_pp_exports[unit].back();
	ev.name = name;
	ev.tag = PackMacroEvent::peDefineFn;
	ev.macro = m;
	pack_macro_origin[name] = PackMacroOrigin{ unit, true };
    }
}

void Program::pack_record_undef(const std::string &name)
{
    if ( const char *unit = pack_current_unit() )
    {
	pack_note_unit(unit);
	pack_pp_exports[unit].push_back(PackMacroEvent());
	PackMacroEvent &ev = pack_pp_exports[unit].back();
	ev.name = name;
	ev.tag = PackMacroEvent::peUndef;
	pack_macro_origin[name] = PackMacroOrigin{ unit, false };
    }
}

void Program::pack_record_edge(const std::string &includee)
{
    if ( const char *unit = pack_current_unit() )
    {
	pack_note_unit(unit);
	pack_unit_edges[unit].push_back(intern_file(includee));
    }
}

void Program::pack_record_branch_macro(const std::string &name,
				       bool include_probe)
{
    if ( !pack_recording || name.empty() )
	return;
    pack_branch_macros.insert(name);
    // An include-once GUARD PROBE (should_tokenize_include asking "is the
    // includee's guard defined") is an EDGE decision — reproduced by the
    // frozen edges + include-once semantics at bind, never a branch
    // dependency of the includer's CONTENT. Recording it made every libc++
    // header carry its includees' guards as deps and decline on re-binds.
    if ( include_probe )
	return;
    // v40 (task #57): record the consult as an EXTERNAL branch dependency of
    // the current unit when the macro's state was NOT established within the
    // consulting unit's OWN subtree — consumers bind ANY unit directly, so a
    // sibling under the same top-level include (mingw stdlib.h defining
    // _CRT_ERRNO_DEFINED before errno.h parsed under <cstdlib>) is just as
    // external as an earlier root. Self/subtree definitions are reproduced by
    // the unit's own bind replay; a defined macro with NO recorded origin is
    // predefine/-D state, pinned by the v27 config word — neither is a
    // dependency. The DEFINER travels with the dep: the bind walk skips any
    // dep whose definer is inside the closure being bound (an ANCESTOR's
    // guard — sys/cdefs.h consulting _FEATURES_H under features.h — is
    // replay-internal there, and a real external elsewhere).
    const char *unit = pack_current_unit();
    if ( !unit )
	return;
    bool defined_now = macro_name_defined(name);
    std::map<std::string, PackMacroOrigin>::iterator oi =
	pack_macro_origin.find(name);
    if ( defined_now && oi == pack_macro_origin.end() )
	return;				// predefine / -D: config-word-pinned
    if ( oi != pack_macro_origin.end() && oi->second.unit )
    {
	if ( oi->second.unit == unit )
	    return;			// self: the unit's own replay decides
	std::map<const char *, std::set<const char *> >::iterator sti =
	    pack_unit_subtree.find(unit);
	if ( sti != pack_unit_subtree.end()
	  && sti->second.count(oi->second.unit) )
	    return;			// subtree: edges install it before us
    }
    if ( !pack_unit_branch_dep_seen[unit].insert(name).second )
	return;
    PackBranchDep dep;
    dep.name = name;
    dep.defined = defined_now;
    dep.has_value = false;
    dep.definer = oi != pack_macro_origin.end() ? oi->second.unit : NULL;
    if ( defined_now )
    {
	if ( std::string *dv = define_map.find(name) )
	{
	    dep.has_value = true;
	    dep.value = *dv;
	}
	else if ( MacroDef *mv = macro_map.find(name) )
	{
	    dep.has_value = true;
	    dep.value = mv->body;
	}
    }
    pack_unit_branch_deps[unit].push_back(dep);
}

// -dM: dump the effective macro table (object-like + function-like) in
// `#define` form, sorted by name — the gcc `-dM -E` analogue backing the
// forest PP parity oracle (design doc §7).
void Program::dump_macros(FILE *out)
{
    std::map<std::string, std::string> lines;
    define_map.for_each([&](const char *key, std::string &value) -> bool {
	lines[key] = value.empty() ? std::string() : (" " + value);
	return false;
    });
    macro_map.for_each([&](const char *key, MacroDef &m) -> bool {
	std::string sig = "(";
	for ( size_t i = 0; i < m.params.size(); ++i )
	{
	    if ( i ) sig += ", ";
	    sig += m.params[i];
	    if ( m.variadic && !m.variadic_param.empty() && m.variadic_param == m.params[i] )
		sig += "...";
	}
	if ( m.variadic && m.variadic_param.empty() )
	{
	    if ( !m.params.empty() ) sig += ", ";
	    sig += "...";
	}
	sig += ")";
	lines[key] = sig + (m.body.empty() ? std::string() : (" " + m.body));
	return false;
    });
    for ( std::map<std::string, std::string>::const_iterator it = lines.begin();
	  it != lines.end(); ++it )
	fprintf(out, "#define %s%s\n", it->first.c_str(), it->second.c_str());
}

bool Program::should_tokenize_include(const std::string &path)
{
    // Same canonicalization the search-dir prefixes get — they are compared
    // against each other, so one owner (canonical_path_for_compare) or they drift.
    std::string canonical = path;
    if ( !path.empty() && path[0] != '<' )
	canonical = canonical_path_for_compare(path);
    if ( !path.empty() && path[0] == '<' )
    {
	// Named (embedded/PCH) include keys: blanket once-only — the baked
	// sets assume single inclusion and the surviving embedded headers
	// carry no #ifndef guards of their own.
	if ( include_already_seen(canonical) )
	    return false;
	included_files[canonical] = true;
	return live_tokenize_record(canonical, true);
    }
    // User ("...") includes keep madc's dialect require-once semantics
    // (pinned by tests/testincludeonce.mad). SYSTEM headers get gcc's
    // multiple-include semantics below — they are written for gcc and
    // rely on it (bits/mathcalls.h).
    if ( !is_system_header_path(canonical.c_str()) )
    {
	if ( include_already_seen(canonical) )
	    return false;
	included_files[canonical] = true;
	return live_tokenize_record(canonical, true);
    }
    // System headers: gcc's multiple-include optimization. Skip a
    // repeat inclusion ONLY when the file is fully wrapped in an include
    // guard whose macro is (still) defined. A guard-less header (glibc's
    // bits/mathcalls.h, multi-included with a different _Mdouble_ per
    // pass) is re-tokenized every time, exactly like gcc.
    auto gi = include_guard_by_file.find(canonical);
    if ( gi == include_guard_by_file.end() )
    {
	const std::string guard = detect_include_guard(canonical);
	include_guard_by_file[canonical] = guard;
	if ( guard.empty() )
	    return live_tokenize_record(canonical, true);
	// A FIRST live visit can still be a re-include: a forest bind may
	// already have installed this header's guard (v40 mixed bind/live
	// TUs — a bound <cstdio> then a declined <locale> live-parsing
	// bits/types.h beside the restored decls). Same gcc rule as the
	// repeat path below: guard defined = skip.
	pack_record_branch_macro(guard, true /* include probe */);
	return live_tokenize_record(canonical,
	    define_map.find(guard) == define_map.end()
	    && macro_map.find(guard) == macro_map.end());
    }
    const std::string &guard = gi->second;
    if ( guard.empty() )
	return live_tokenize_record(canonical, true);
    pack_record_branch_macro(guard, true /* include probe */);	// B4a: guard definedness gates inclusion
    return live_tokenize_record(canonical,
	define_map.find(guard) == define_map.end()
	&& macro_map.find(guard) == macro_map.end());
}

// One recording owner: a TRUE verdict from should_tokenize_include means the
// file's tokens enter THIS TU's live stream now. The v40 prune's honest
// "already present" test (forest_unit_file_live_tokenized) reads the set —
// a live guard macro alone is NOT sufficient evidence (a shared sub-block
// guard or a bound sibling's replay also defines it: mingw stdio.h's
// _FILE_DEFINED, the embedded stddef's NULL).
bool Program::live_tokenize_record(const std::string &canonical, bool tok)
{
    if ( tok )
	forest_live_tokenized.insert(canonical);
    return tok;
}

// v40 prune evidence: was this frozen unit's FILE live-tokenized earlier in
// this TU? Filesystem units freeze under their resolved (possibly ../-laden)
// paths — compare canonically, the same owner the live record used. Embedded
// units freeze under bare names ("stddef.h"); their live record is the
// angle-bracket named key ("<stddef.h>").
bool Program::forest_unit_file_live_tokenized(uint32_t unit)
{
    const char *uname = bind_forest->unit_name(unit);
    if ( !uname || !*uname )
	return false;
    if ( *uname == '<' )
	return forest_live_tokenized.count(uname) != 0;
    if ( !strchr(uname, '/') )
	return forest_live_tokenized.count("<" + std::string(uname) + ">") != 0
	    || forest_live_tokenized.count(uname) != 0;
    return forest_live_tokenized.count(canonical_path_for_compare(uname)) != 0;
}

// Defined here, used from tokenize_posix_whole_provider above too (declared
// with it). "Does this path name a readable file" has ONE owner in this file.
bool madc_lexer_file_exists(const std::string &path)
{
    std::ifstream probe(path.c_str(), std::ios::binary);
    return probe.good();
}

// A resolved include can be readable from either ordinary storage or the raw
// source slot in a packed forest.  This is the one existence predicate AFTER
// resolution: consumers must not mistake a carrier-backed native header for a
// missing provider and let a lower-priority compatibility header replace it.
bool Program::resolved_include_provider_exists(const std::string &path)
{
    if ( path.empty() )
	return false;
    if ( madc_lexer_file_exists(path) )
	return true;
    std::string frozen_path;
    return forest_source_path(path, /*allow_tail=*/false, frozen_path);
}

// The READING twin of resolved_include_provider_exists(): a resolved include's
// BYTES come from ordinary storage or the packed forest's raw-source slot, in
// that order. Both facts live here so no consumer can disagree with another
// about whether a header is readable.
//
// That disagreement was a real defect, not a hypothetical. detect_include_guard()
// read only from disk and returned "" when it could not open the file — and ""
// is also how it spells "this header has no include guard" (glibc's
// bits/mathcalls.h, deliberately multi-included). On a compiler-less target the
// two became indistinguishable, so gcc's multiple-include optimization inverted:
// every forest-served header looked guard-less, got re-tokenized instead of
// skipped, and the re-tokenize then failed to open it. 67 suite tests died on
// one header (<stdc-predef.h>, whose guard madc predefines exactly so it can be
// skipped). Read failure and absent-guard are different answers; only a shared
// reader keeps them apart.
bool Program::read_resolved_include(const std::string &path, std::string &text)
{
    if ( path.empty() )
	return false;
    std::ifstream in(path.c_str(), std::ios::binary);
    if ( in )
    {
	std::ostringstream tmp;
	tmp << in.rdbuf();
	text = tmp.str();
	return true;
    }
    return forest_source_text(path, text);
}

static void add_pch_candidate(std::vector<std::string> &candidates,
			      const std::string &dir,
			      const std::string &incfile)
{
    std::string path = dir;
    if ( !path.empty() && path.back() != '/' )
	path += '/';
    path += incfile;
    path += ".madh";
    candidates.push_back(path);
}

static bool find_filesystem_precompiled_header(Program &pgm,
					       const std::string &incfile,
					       bool is_system,
					       std::string &outpath)
{
    std::vector<std::string> candidates;
    if ( !incfile.empty() && incfile[0] == '/' )
	candidates.push_back(incfile + ".madh");
    else
    {
	std::string cur_dir = pgm.current_source_directory();
	if ( !is_system && !cur_dir.empty() )
	    add_pch_candidate(candidates, cur_dir, incfile);
	for ( size_t i = 0; i < pgm.include_paths.size(); ++i )
	    add_pch_candidate(candidates, pgm.include_paths[i], incfile);
	if ( is_system && !cur_dir.empty() )
	    add_pch_candidate(candidates, cur_dir, incfile);
    }

    for ( size_t i = 0; i < candidates.size(); ++i )
    {
	if ( madc_lexer_file_exists(candidates[i]) )
	{
	    outpath = candidates[i];
	    return true;
	}
    }
    return false;
}

// One named-provider probe shared by the explicit-include path and the
// auto-include defer gate: can <incfile> be served without a filesystem
// walk — a filesystem .madh, a baked PCH, or an embedded text header?
// On success pch_path names the filesystem .madh (empty for the others).
static bool named_include_provider_exists(Program &pgm,
					  const std::string &incfile,
					  std::string &pch_path)
{
    if ( find_filesystem_precompiled_header(pgm, incfile, true, pch_path) )
	return true;
    if ( find_precompiled_header(incfile) )
	return true;
    // The embedded arm answers only when nothing OUTRANKS the embedded copy —
    // the embedded set sits at madc's compiler-resource-dir slot, so a C++
    // standard library (or any -I dir) ahead of that slot supplies the name
    // instead. Gated here as well as at the tokenize site so the once-only
    // dedup key and the auto-include defer gate agree with what actually
    // resolves; a name they disagreed about would be dropped silently.
    return find_embedded_header(incfile) != NULL
	&& pgm.is_embedded_header_allowed(incfile)
	&& !pgm.embedded_header_outranked(incfile);
}

static bool load_precompiled_header_file(const std::string &path,
					 std::deque<TokenBase *> &tokens)
{
    std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
    if ( !in )
	return false;
    std::streampos end = in.tellg();
    if ( end <= 0 )
	return false;
    size_t size = (size_t)end;
    in.seekg(0);
    std::vector<uint8_t> bytes(size);
    in.read((char *)bytes.data(), size);
    if ( !in )
	return false;
    return madc_pch::read_madh(bytes.data(), bytes.size(), tokens);
}

static bool push_precompiled_header_tokens(Program &pgm,
					   const std::string &display_name,
					   std::deque<TokenBase *> &pch_tokens)
{
    const char *interned = pgm.intern_file(display_name);
    pgm.pack_record_edge(display_name);	// B4a: includer (current source) -> PCH unit
    pgm.pack_note_unit(interned);
    for ( TokenBase *itb : pch_tokens )
    {
	if ( TokenIdent *ident = dynamic_cast<TokenIdent *>(itb) )
	{
	    TokenBase *replacement = NULL;
	    keyword_map_iter ki = pgm.keyword_map.find(ident->spelling());
	    if ( ki != pgm.keyword_map.end() )
		replacement = (*ki)->clone();
	    else
	    {
		flat_datatype_map_iter di = pgm.datatype_map.find(ident->spelling());
		if ( di != pgm.datatype_map.end() )
		    replacement = (*di)->clone();
	    }
	    if ( replacement )
	    {
		replacement->line = itb->line;
		replacement->column = itb->column;
		delete itb;
		itb = replacement;
	    }
	}
	itb->file = interned;
	pgm.push_token_with_literal_concat(itb);
    }
    return true;
}

// add static tokens for language keywords
void Program::add_keywords()
{
    keyword_map[tkDO.str] = &tkDO;
    keyword_map[tkIF.str] = &tkIF;
    keyword_map[tkFOR.str] = &tkFOR;
    keyword_map[tkELSE.str] = &tkELSE;
    keyword_map[tkRETURN.str] = &tkRETURN;
    keyword_map[tkGOTO.str] = &tkGOTO;
    keyword_map[tkCASE.str] = &tkCASE;
    keyword_map[tkBREAK.str] = &tkBREAK;
    keyword_map[tkCONT.str] = &tkCONT;
    if ( !is_c_mode() ) {
	keyword_map[tkTRY.str] = &tkTRY;
	keyword_map[tkCATCH.str] = &tkCATCH;
	keyword_map[tkTHROW.str] = &tkTHROW;
    }
    keyword_map[tkSWITCH.str] = &tkSWITCH;
    keyword_map[tkWHILE.str] = &tkWHILE;
    if ( !is_c_mode() )
	keyword_map[tkCLASS.str] = &tkCLASS;
    keyword_map[tkSTRUCT.str] = &tkSTRUCT;
    keyword_map[tkUNION.str] = &tkUNION;
    keyword_map[tkDEFAULT.str] = &tkDEFAULT;
    keyword_map[tkTYPEDEF.str] = &tkTYPEDEF;
    keyword_map[tkOPEROVER.str] = &tkOPEROVER;
    keyword_map[tkREGISTER.str] = &tkREGISTER;
    keyword_map[tkSTATIC.str] = &tkSTATIC;
    keyword_map[tkENUM.str] = &tkENUM;
    keyword_map[tkCONST.str] = &tkCONST;
    keyword_map[tkEXTERN.str] = &tkEXTERN;
    keyword_map[tkRESTRICT.str] = &tkRESTRICT;
    keyword_map[tkVOLATILE.str] = &tkVOLATILE;
    keyword_map["__volatile"] = &tkVOLATILE;
    keyword_map["__volatile__"] = &tkVOLATILE;
    if ( !is_c_mode() ) {
	keyword_map[tkUSING.str] = &tkUSING;
	keyword_map[tkNAMESPACE.str] = &tkNAMESPACE;
	keyword_map[tkPREFER.str] = &tkPREFER;
	keyword_map[tkTEMPLATE.str] = &tkTEMPLATE;
	keyword_map[tkNEW.str] = &tkNEW;
	keyword_map[tkDELETE.str] = &tkDELETE;
	keyword_map[tkFRIEND.str] = &tkFRIEND;
    }
    keyword_map[tkDEFER.str] = &tkDEFER;

    // Version-gated C++ RESERVED-keyword registry (C++26 and earlier). Each
    // entry is reserved ONLY in the madc dialect or an explicit C++ mode at/after
    // its introducing standard (cpp_keyword_active) — NEVER in C. These have no
    // dedicated dispatch token: the parser already recognizes them by SPELLING
    // (contextual_identifier_name), and tkCPPKEYWORD is admitted to that helper's
    // allowlist — so reserving them is transparent to existing parse sites while
    // preventing their use as bare identifiers.
    //
    // ONLY genuine reserved keywords appear here. Contextual identifiers
    // (`override`, `final`, `module`, `import`, `audit`) are deliberately NOT
    // reserved (a hard reservation broke 49 tests — see the KG lesson). The
    // pervasive ignored specifier `alignas` (special lexer handling) keeps its
    // existing treatment. The erased specifiers `constexpr`/`consteval`/
    // `constinit` are registered below AFTER being removed from the erase map,
    // and need decl-specifier consume handling; `inline` and `noexcept` keep
    // their erasure in NON-C++ modes only.
    struct CppReservedKw { const char *kw; LanguageStd min_std; };
    static const CppReservedKw cpp_reserved[] = {
	// STAGED — see DESIGN NOTE / the plan. The complete reserved set (below,
	// commented) is validated-but-not-yet-activated: hard-reserving them is a
	// genuine multi-site de-shim (every direct `type()==ttIdentifier` check
	// that must accept the token), and a full activation surfaced 9 regressions
	// (asm-statement dispatch, the move/forward template-instantiation reparse,
	// a __x scope-loss, math.h + string operator+ codegen). Activate in
	// validated slices per docs/plans/2026-06-15-cpp-keyword-registry-plan.md.
	//   C++98: this typename sizeof typeid true false
	//          static_cast const_cast reinterpret_cast dynamic_cast
	//   C++11: decltype alignof nullptr static_assert thread_local
	// --- C++20 — DEFERRED (NOT yet reserved). madc presents as a C++20+
	//     dialect to real headers, which use `concept`/`requires` (active
	//     under __cpp_lib_concepts, e.g. <compare>/<concepts>) and the
	//     coroutine keywords. madc lacks concept/coroutine PARSING; today it
	//     SKIPS those declarations via string-gated paths (skip_requires_clause
	//     &al.). Reserving these as tokens breaks those skip paths, so they
	//     stay contextual until the skip paths are de-shimmed to accept the
	//     tokens. Listed here so the registry is COMPLETE/accounted-for:
	//       char8_t, concept, requires, co_await, co_return, co_yield  (C++20)
	//     Tracked in docs/plans/2026-06-15-cpp-keyword-registry-plan.md.
	//
	// --- ACTIVATED SLICES (validated, zero-regression) ---
	// Slice 1 (asm): the standard C++ keyword `asm`. Statement-position
	// asm is skipped by the shared Program::skip_gnu_asm_statement, reached
	// from BOTH the ttIdentifier and ttKeyword parseStatement arms; asm
	// labels on declarations go through consume_gnu_asm_label (dynamic_cast
	// to TokenIdent, works for the keyword token). The GNU spellings
	// `__asm__`/`__asm` stay contextual (double-underscore impl-reserved).
	{ "asm",              STD_CPP98 },
	// Slice 2 (declaration keywords): access specifiers and member/base
	// specifiers. Every parse site reads them via
	// is_contextual_identifier_token / contextual_identifier_name (base-spec
	// virtual/access loop, access-label public/private/protected, the
	// member-specifier virtual/mutable/explicit loop, and the has-methods
	// detector) — all already admit tkCPPKEYWORD. `export` has no dedicated
	// handler (export-template was removed in C++11; C++20 module `export`
	// does not appear in the classic headers madc parses), so it is reserved
	// for completeness only.
	{ "explicit",         STD_CPP98 },
	{ "mutable",          STD_CPP98 },
	{ "virtual",          STD_CPP98 },
	{ "export",           STD_CPP98 },
	{ "public",           STD_CPP98 },
	{ "private",          STD_CPP98 },
	{ "protected",        STD_CPP98 },
	// Slice 3 (typename) — DEFERRED (staged, NOT reserved). Every direct
	// parse site already reads contextual_identifier_name / TokenIdent::str
	// (so `template<typename T>` and `typename X::type{...}` are fine), but
	// reserving it regresses the LATE free-function-template instantiation of
	// std::move / std::forward: `int&& y = std::move(x)` emits `&__ns_std_move`
	// with the `(x)` call DROPPED (emitted-C shows `int *y = (&__ns_std_move)`),
	// i.e. the move/forward template instantiation is not triggered at the call
	// site. The bug is in the free-fn-template return-type (`typename
	// std::remove_reference<_Tp>::type&&`) instantiation/reparse path, not a
	// plain ttIdentifier de-shim. Reproduce with tmp/fwd.mad. Reserve only
	// after that path is fixed (testmemtmplpackexpand, testlateinstproto).
	// Slice 5 (constexpr): a real tkCPPKEYWORD (no longer erased) so the
	// dormant TokenIF `if constexpr` discard machinery activates. As an
	// ignored decl-specifier it is consumed by TokenCppKeyword::parse (leading
	// and storage-delegated `static constexpr` / `const constexpr`) and the
	// member-specifier loop; is_ignored_cpp_specifier_token recognizes it.
	{ "constexpr",        STD_CPP11 },
	// Slice 6 (consteval/constinit, C++20): ignored decl-specifiers, handled
	// by the same is_ignored_cpp_specifier_token path as constexpr.
	{ "consteval",        STD_CPP20 },
	{ "constinit",        STD_CPP20 },
	// inline (un-erased 2026-07-24, ELF-completion S4 follow-through): a
	// real decl-specifier consumed by TokenCppKeyword::parse (which also
	// owns `inline namespace`) and the member-specifier loop; it carries
	// vague linkage — bodied external-linkage functions/variables it
	// qualifies emit linkonce (STB_WEAK) so per-TU header copies merge at
	// native links. C modes keep the erasure (see _tokenizer_init).
	{ "inline",           STD_CPP98 },
	// Slice 4 (expression keywords) — validating subset first. The named
	// casts / typeid / decltype / alignof are recognized by spelling in
	// parse_constant_primary and the expression parser (de-shimmed), and are
	// implausible as identifiers. `this`, `sizeof`, `nullptr`, `true`,
	// `false` are staged separately (SESSION-16 §4 flagged semantic regressions).
	{ "static_cast",      STD_CPP98 },
	{ "const_cast",       STD_CPP98 },
	{ "reinterpret_cast", STD_CPP98 },
	{ "dynamic_cast",     STD_CPP98 },
	{ "typeid",           STD_CPP98 },
	{ "decltype",         STD_CPP11 },
	{ "alignof",          STD_CPP11 },
	// noexcept — BOTH surfaces ([expr.unary.noexcept] operator and the
	// [except.spec] specifier — un-erased 2026-08-04): the operator folds by
	// spelling in parse_constant_primary and the expression parser (like
	// sizeof/alignof); the specifier is captured by parseFunction's
	// trailing-qualifier walk (NxTrue/NxNone/NxUnknown). Non-C++ modes keep
	// the getToken() balanced-paren erasure (mirror of inline's C-mode split).
	{ "noexcept",         STD_CPP11 },
	// Slice 4b: boolean / pointer literals.
	{ "true",             STD_CPP98 },
	{ "false",            STD_CPP98 },
	{ "nullptr",          STD_CPP11 },
	// Slice 4c: sizeof (bisecting — SESSION-16 §4 flagged the expr-keyword set).
	{ "sizeof",           STD_CPP98 },
	// Slice 4d: this (bisecting — madc models the receiver as __this).
	{ "this",             STD_CPP98 },
	{ 0,                  STD_CPP98 }
    };
    for ( size_t i = 0; i < sizeof(cpp_reserved)/sizeof(cpp_reserved[0]); ++i )
	if ( cpp_reserved[i].kw
	  && cpp_keyword_active(cpp_reserved[i].min_std)
	  && keyword_map.find(cpp_reserved[i].kw) == keyword_map.end() )
	    keyword_map[cpp_reserved[i].kw] =
		new TokenCppKeyword(cpp_reserved[i].kw);

    // Slice 7 — alternative-token operators ([lex.digraph]). In C++ these are
    // reserved keywords spelled as words; each is an exact synonym for a
    // symbolic operator, so map the spelling straight to the operator token
    // (cloned on lookup at lexer.cpp ~4382, like every keyword). C++98+ /
    // madc-dialect only (cpp_keyword_active): in C they are NOT keywords —
    // <iso646.h> defines them as macros, which the real header supplies.
    if ( cpp_keyword_active(STD_CPP98) )
    {
	cpp_operator_map["and"]    = new TokenLand();    // &&
	cpp_operator_map["or"]     = new TokenLor();     // ||
	cpp_operator_map["not"]    = new TokenLnot();    // !
	cpp_operator_map["bitand"] = new TokenBand();    // &
	cpp_operator_map["bitor"]  = new TokenBor();     // |
	cpp_operator_map["xor"]    = new TokenXor();     // ^
	cpp_operator_map["compl"]  = new TokenBnot();    // ~
	cpp_operator_map["not_eq"] = new TokenNotEq();   // !=
	cpp_operator_map["and_eq"] = new TokenBandEq();  // &=
	cpp_operator_map["or_eq"]  = new TokenBorEq();   // |=
	cpp_operator_map["xor_eq"] = new TokenXorEq();   // ^=
    }
    // DESIGN NOTE: this table is intentionally NOT populated with hard keyword
    // tokens. madc deliberately handles most C++ keywords as CONTEXTUAL
    // identifiers recognized by spelling (Cfront-style; see the KG lesson
    // "Tokenizer remapping for contextual keywords" — a grammar-level hard
    // reservation caused a 49-test regression). Empirically, reserving e.g.
    // `typename` as a tkCPPKEYWORD breaks real-header `template<typename ...>`
    // parsing. Reservation + version-gating must therefore be expressed as a
    // gated SPELLING predicate consulted by the contextual sites, not as hard
    // tokens. Tracked in docs/plans/2026-06-15-cpp-keyword-registry-plan.md.
}

// add static tokens for base data types
void Program::add_datatypes()
{
    // Idempotent: globals get the same fixed ABI slot every time, so
    // multiple Programs per process (project driver) are safe.
    madc_stamp_primitive_type_ids();

    static TokenDataType tkPTRDIFF("ptrdiff_t", ddINT64);
    static TokenDataType tkSIZE_T("size_t", ddUINT64);
    // int64_t/uint64_t follow the target's int64 ALIAS spelling (datadef.h
    // TargetInt64Alias): the distinct long-long-shaped dds on darwin (the
    // host exports mangle x/y there), the pinned ddINT64/ddUINT64
    // everywhere else. Same function-static process-fixed binding contract
    // as tkWCHAR_T below. These rows OVERRIDE the static tkINT64/tkUINT64
    // pair, whose compile-time ddINT64 binding cannot follow the target.
    static TokenDataType tkINT64_T("int64_t", *dd_platform_longlong());
    static TokenDataType tkUINT64_T("uint64_t", *dd_platform_ulonglong());
    // wchar_t is target-shaped (int32 LP64 / uint16 LLP64). Function-static:
    // binds the model at the FIRST Program's add_datatypes — fine while the
    // model is fixed per process (hosted modes); a per-Program --target flip
    // would need these statics revisited.
    static TokenDataType tkWCHAR_T("wchar_t", *dd_platform_wchar());
    static TokenDataType tkCHAR8_T("char8_t", ddUINT8);
    static TokenDataType tkCHAR16_T("char16_t", ddUINT16);
    static TokenDataType tkCHAR32_T("char32_t", ddUINT32);
    static TokenDataType tkMAX_ALIGN_T("max_align_t", ddMAX_ALIGN_T);
    // _Float16 rides the same nearest-supported approximation the other
    // _FloatN spellings use (MIR has no half-float): declarations parse;
    // half-precision ABI fidelity is a MIR floor gap (SIMD-class, Tier 3).
    static TokenDataType tkFLOAT16("_Float16", ddFLOAT);
    // __bf16 (bfloat16 — gcc's avx512bf16 headers typedef vectors of it):
    // the same nearest-supported approximation posture as _Float16.
    static TokenDataType tkBF16("__bf16", ddFLOAT);
    static TokenDataType tkFLOAT32("_Float32", ddFLOAT);
    static TokenDataType tkFLOAT64("_Float64", ddDOUBLE);
    static TokenDataType tkFLOAT128("_Float128", ddDOUBLE);
    static TokenDataType tkFLOAT32X("_Float32x", ddDOUBLE);
    static TokenDataType tkFLOAT64X("_Float64x", ddDOUBLE);
    static TokenDataType tkDECIMAL32("_Decimal32", ddFLOAT);
    static TokenDataType tkDECIMAL64("_Decimal64", ddDOUBLE);
    static TokenDataType tkDECIMAL128("_Decimal128", ddDOUBLE);

    datatype_map[tkVOID.str] = &tkVOID;
    datatype_map[tkBOOL.str] = &tkBOOL;
    datatype_map[tkC23BOOL.str] = &tkC23BOOL;
    datatype_map[tkCHAR.str] = &tkCHAR;
    datatype_map[tkINT.str] = &tkINT;
    datatype_map[tkINT8.str] = &tkINT8;
    datatype_map[tkINT16.str] = &tkINT16;
    datatype_map[tkINT24.str] = &tkINT24;
    datatype_map[tkINT32.str] = &tkINT32;
    datatype_map[tkINT64_T.str] = &tkINT64_T;
    datatype_map[tkUINT8.str] = &tkUINT8;
    datatype_map[tkUINT16.str] = &tkUINT16;
    datatype_map[tkUINT24.str] = &tkUINT24;
    datatype_map[tkUINT32.str] = &tkUINT32;
    datatype_map[tkUINT64_T.str] = &tkUINT64_T;
    datatype_map[tkFLOAT.str] = &tkFLOAT;
    datatype_map[tkDOUBLE.str] = &tkDOUBLE;
    // `array` is madc-dialect-only (slice V1): explicit C/C++ standards
    // keep it an ordinary identifier (SMAUG-class C89 code declares
    // `int array;` freely). Its dialect twins `value` and `var` are
    // deliberately NOT lexer datatype tokens at all — a datatype token
    // hijacks every identifier position, and `value` is ubiquitous in
    // system headers (members, params, template bodies). They resolve
    // through the typedef lane instead: Program::madc_dialect_type_spelling
    // consulted by resolve_declared_type_token and the statement arm.
    if ( language_std == STD_MADC )
	datatype_map[tkARRAY.str] = &tkARRAY;
    datatype_map[tkLPSTR.str] = &tkLPSTR;
    datatype_map[tkAUTO.str] = &tkAUTO;
    datatype_map[tkPTRDIFF.str] = &tkPTRDIFF;
    datatype_map[tkSIZE_T.str] = &tkSIZE_T;
    // wchar_t / char8_t / char16_t / char32_t are FUNDAMENTAL built-in types in
    // C++ (keywords — [basic.fundamental]), but in C they are typedefs supplied
    // by headers (wchar_t: <stddef.h>/<wchar.h>; char16_t/char32_t: <uchar.h>;
    // char8_t: <uchar.h> in C23). So register them as primitives ONLY in the
    // madc dialect or an explicit C++ mode at/after their introducing standard;
    // in explicit C modes the real header typedef supplies them (retire-
    // embedded-shims principle — do not preempt the real header).
    if ( cpp_keyword_active(STD_CPP98) )
	datatype_map[tkWCHAR_T.str] = &tkWCHAR_T;
    if ( cpp_keyword_active(STD_CPP20) )
	datatype_map[tkCHAR8_T.str] = &tkCHAR8_T;
    if ( cpp_keyword_active(STD_CPP11) )
    {
	datatype_map[tkCHAR16_T.str] = &tkCHAR16_T;
	datatype_map[tkCHAR32_T.str] = &tkCHAR32_T;
    }
    datatype_map[tkMAX_ALIGN_T.str] = &tkMAX_ALIGN_T;
    datatype_map[tkFLOAT16.str] = &tkFLOAT16;
    datatype_map[tkBF16.str] = &tkBF16;
    datatype_map[tkFLOAT32.str] = &tkFLOAT32;
    datatype_map[tkFLOAT64.str] = &tkFLOAT64;
    datatype_map[tkFLOAT128.str] = &tkFLOAT128;
    datatype_map[tkFLOAT32X.str] = &tkFLOAT32X;
    datatype_map[tkFLOAT64X.str] = &tkFLOAT64X;
    datatype_map[tkDECIMAL32.str] = &tkDECIMAL32;
    datatype_map[tkDECIMAL64.str] = &tkDECIMAL64;
    datatype_map[tkDECIMAL128.str] = &tkDECIMAL128;

    // Everything registered above is one of madc's OWN base datatypes, never a
    // user declaration. Marking the whole map here keeps that automatic — a
    // future builtin needs no second edit — and lets the typedef paths accept a
    // real header re-declaring one of these names (gcc's <stddef.h> carries
    // `typedef struct {...} max_align_t;`) without weakening the genuine
    // user-vs-user redefinition diagnostic.
    datatype_map.for_each([](const char *, TokenDataType *&tdt) {
	if ( tdt )
	    tdt->builtin = true;
	return false;	// visit every entry
    });
}


// CHAR-LEVEL directive-line-tail skip, used ONLY while skipping an inactive
// #if branch (skipConditionalBlock). The active path uses
// consume_directive_line_tail() to run the tail through the lexer (reusing its
// comment handling), but inactive content must NOT be tokenized or macro-
// expanded — so here we scan raw characters, mirroring the lexer's comment rule
// by hand: a `/* */` block comment counts as whitespace and may span physical
// newlines, so skip it IN FULL (else its continuation lines leak as directives/
// code). Stop at the first newline NOT inside a block comment; `//` ends the line.
static void skip_directive_line_tail(Source &source)
{
    while ( source.good() && !source.eof() )
    {
	int c = source.peek();
	if ( c == '\n' || c == '\r' )
	    break;
	if ( c == '/' )
	{
	    source.get();
	    int n = source.peek();
	    if ( n == '*' )
	    {
		source.get();
		int prev = 0;
		while ( source.good() && !source.eof() )
		{
		    int cc = source.get();
		    if ( prev == '*' && cc == '/' )
			break;
		    prev = cc;
		}
		continue;
	    }
	    if ( n == '/' )
	    {
		while ( source.good() && !source.eof()
		     && source.peek() != '\n' && source.peek() != '\r' )
		    source.get();
		break;
	    }
	    continue;   // a lone '/', already consumed
	}
	source.get();
    }
}

// === pop-1 lexer-token factory (token-arena Phase 2 seam, step 2.2a) ===
// Single construction point for every lexed (pop-1) token. 2.2a is
// behavior-identical: still heap-`new`s and returns a TokenBase*, no
// representation change. 2.2b makes these append a POD TokenRec to the arena
// and return a slot-id-backed shell. Payload-free kinds delegate to the shared
// madc_pch::token_from_id switch (ONE materialize-from-kind table); payload
// kinds construct directly. See docs/plans/2026-06-23-p1-token-arena-*.
TokenBase *Program::make_token(TokenID kind)
{
    TokenBase *tb = madc_pch::token_from_id(kind);
    // token_from_id materializes whitespace with count 1; the lexer never
    // routes Space/Tab/EOL here (they carry a count -> make_space/tab/eol).
    return tb;
}

TokenBase *Program::make_ident(const std::string &spelling)
{
    TokenIdent *t = new TokenIdent(spelling.c_str());
    t->rec.spelling_id = strpool.intern(spelling);	// interned at creation (Step 4)
    return t;
}

TokenBase *Program::make_int(int64_t value)
{
    return new TokenInt(value);
}

TokenBase *Program::make_int(int64_t value, const std::string &src)
{
    return new TokenInt(value, src);
}

TokenBase *Program::make_real(long double value)
{
    return new TokenReal(value);
}

TokenBase *Program::make_str(const std::string &bytes, bool wide)
{
    return new TokenStr(bytes, wide);
}

TokenBase *Program::make_char(int code)
{
    return new TokenChar(code);
}

TokenBase *Program::make_datatype(const char *name, DataDef &dd)
{
    TokenDataType *t = new TokenDataType(name, dd);
    t->rec.spelling_id = strpool.intern(name);		// interned at creation (Step 4)
    return t;
}

TokenBase *Program::make_rem(const std::string &text)
{
    return new TokenREM(text);
}

TokenBase *Program::make_space(int cnt) { return new TokenSpace(cnt); }
TokenBase *Program::make_tab(int cnt)   { return new TokenTab(cnt); }
TokenBase *Program::make_eol(int cnt)   { return new TokenEOL(cnt); }

// lex and return the next token from the data stream
// TODO: replace top switch with direct dispatch
//       also likely better to replace istream stuff
//       with a character buffer for maximum speed
TokenBase *Program::_getToken()
{
    keyword_map_iter kmi;
    flat_datatype_map_iter bmi = nullptr;
    string word;
    int ch, cnt, row, col;

    // Runaway-expansion guard. macro_disabled() blue paint stops the ordinary
    // recursive-macro cases, but a depth bound is a cheap backstop that turns
    // ANY future runaway (a re-expansion the paint misses, a pathological
    // include/macro chain) into a clean diagnostic instead of a stack crash.
    // One pushback frame per live macro expansion; legitimate nesting is tiny
    // (chains of a few dozen), so a high bound never trips on real code.
    if ( source.pushback_depth() > 4096 )
	Throw << "macro/preprocessor expansion nested too deeply ("
	      << source.pushback_depth()
	      << ") — likely a recursive macro" << flush;

    if ( !injected_tokens.empty() )
    {
	TokenBase *tb = injected_tokens.front();
	injected_tokens.pop_front();
	return tb;
    }

    if ( !source.good() || source.eof() ) { return NULL; }

    ch = source.get();
    if ( ch == -1 ) { return NULL; }  // EOF after last char (no trailing newline)
    switch( ch )
    {
	// Form feed and vertical tab are whitespace (C11 5.4 / [lex.charset]).
	// glibc headers use lone ^L page separators (regex.h, bits/mman*.h);
	// falling through to the char-token default desynced the whole
	// following parse ("unexpected token type 10" — the wall-4 family).
	case ' ':
	case '\f':
	case '\v':
	    cnt = 1;
	    while ( source.peek() == ' ' || source.peek() == '\f'
		 || source.peek() == '\v' )
	    {
		++cnt;
		source.get();
		if ( !source.good() || source.eof() )
		    break;
	    }
	    return make_space(cnt);
	case '\t':
	    cnt = 1;
	    while ( source.peek() == '\t' )
	    {
		++cnt;
		source.get();
		if ( !source.good() || source.eof() )
		    break;
	    }
	    return make_tab(cnt);
	case '\r':
	    source.get();
	case '\n':
	    cnt = 1;
	    while ( source.peek() == '\r' || source.peek() == '\n' )
	    {
		++cnt;
		if ( source.peek() == '\r' ) { source.get(); }
		source.get();
		if ( !source.good() || source.eof() )
		    break;
	    }
	    return make_eol(cnt);
	case '=':
	    if (source.peek() == '=')
	    {
		source.get();
		// === is the madc dialect's strict-equality token. Below the
		// std floor the sequence lexes as == then = — C/C++ sources
		// keep their conforming syntax error.
		if (source.peek() == '=' && language_std == STD_MADC)
		    { source.get(); return make_token(TokenID::tk3Eq); }		// ===
		return make_token(TokenID::tkEquals);					// ==
	    }
	    if (source.peek() == '>') { source.get(); return make_token(TokenID::tkFatArrow); } // =>
	    return make_token(TokenID::tkAssign);					// =
	case '+':
	    if (source.peek() == '+') { source.get(); return make_token(TokenID::tkInc);   }   // ++
	    if (source.peek() == '=') { source.get(); return make_token(TokenID::tkAddEq); }   // +=
	    return make_token(TokenID::tkPlus);					// +
	case '-':
	    if (source.peek() == '-') { source.get(); return make_token(TokenID::tkDec);   }   // --
	    if (source.peek() == '=') { source.get(); return make_token(TokenID::tkSubEq); }   // -=
	    if (source.peek() == '>') { source.get(); return make_token(TokenID::tkDeRef); }   // ->
	    return make_token(TokenID::tkNeg);					// -
	case '*': if (source.peek() != '=') return make_token(TokenID::tkMul);		// *
	     source.get(); return make_token(TokenID::tkMulEq);				// *=
	case '/':
	    if (source.peek() == '=') { source.get(); return make_token(TokenID::tkDivEq); }   // /=
	    if (source.peek() == '/')					// //
	    {
		source.get();
		word = "//";
		while ( source.good() && !source.eof() && source.peek() != '\r' && source.peek() != '\n' )
		    word += source.get();
		return make_rem(word);
	    }
	    if (source.peek() == '*')					// /*
	    {
		source.get();
		word = "/*";
		while ( source.good() && !source.eof() )
		{
		    ch = source.get();
		    if ( ch == '*' && source.peek() == '/' )		// */
		    {
			word += ch;
			word += source.get();
			break;
		    }
		    word += ch;
		}
		return make_rem(word);
	    }
	    return make_token(TokenID::tkSlash);
	case '\\': return make_token(TokenID::tkBslsh);
	case '#': // #! is a special comment style for shell script execution
	    if ( source.peek() == '!' )
	    {
		source.get();
		word = "#!";
		while ( source.good() && !source.eof() && source.peek() != '\r' && source.peek() != '\n' )
		    word += source.get();
		// Parse shebang args if interpreter is madc
		{
		    size_t sp = word.find(' ');
		    std::string path = (sp != std::string::npos) ? word.substr(2, sp - 2) : word.substr(2);
		    std::string base = madc::detail::host_path_basename(path);
		    if ( base == "madc" && sp != std::string::npos )
		    {
			std::string args = word.substr(sp + 1);
			std::istringstream as(args);
			std::string arg;
			while ( as >> arg )
			    if ( set_language_standard_option(arg) )
				break;
			// Remove C++ keywords retroactively if shebang set C mode
			if ( is_c_mode() )
			{
			    keyword_map.erase("try");
			    keyword_map.erase("catch");
			    keyword_map.erase("throw");
			    keyword_map.erase("class");
			    keyword_map.erase("new");
			    keyword_map.erase("delete");
			    keyword_map.erase("using");
			    keyword_map.erase("namespace");
			    keyword_map.erase("prefer");
			}
		    }
		}
		return make_rem(word);
	    }
	    while ( source.peek() == ' ' || source.peek() == '\t' )
		source.get();
	    // #include directive
	    if ( isalpha(source.peek()) )
	    {
		std::string directive;
		// '_' is accepted so the compound directive #include_next is
		// read whole (no standard directive but include_next uses '_').
		while ( source.good() && !source.eof()
		     && (isalpha(source.peek()) || source.peek() == '_') )
		    directive += source.get();
		if ( directive == "include" || directive == "include_next" )
		{
		    bool is_include_next = (directive == "include_next");
		    // skip_includes mode: consume rest of line and continue
		    if ( skip_includes )
		    {
			while ( source.good() && !source.eof()
			     && source.peek() != '\n' && source.peek() != '\r' )
			    source.get();
			return getToken();
		    }
		    // skip whitespace
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    // read filename: "file" or <file>
		    char delim = source.get();
		    char end_delim = (delim == '<') ? '>' : '"';
		    bool is_system = (delim == '<');
		    std::string incfile;
		    while ( source.good() && !source.eof() && source.peek() != end_delim
		    &&      source.peek() != '\n' && source.peek() != '\r' )
			incfile += source.get();
		    if ( source.peek() == end_delim )
			source.get(); // consume closing delimiter
		    // Fidelity mode: record the directive AS WRITTEN, paired
		    // with the writing file — the reverse-render (--emit=c++)
		    // re-emits a TU's own directives in place of the expanded
		    // header machinery. Recorded pre-resolution: a once-only
		    // skip still wrote the line.
		    if ( keep_trivia )
			fidelity_include_directives.push_back(std::make_pair(
			    std::string(source.fname()),
			    std::string("#") + directive + " "
			    + (delim == '<' ? "<" : "\"") + incfile
			    + (delim == '<' ? ">" : "\"")));
		    // posix/<name> is a compiler-internal storage namespace. A
		    // user include must name the public native header; otherwise a
		    // supplement could be served without its required real provider.
		    if ( is_system && is_posix_compat_header_name(incfile) )
			Throw << "Failed to open include file: " << incfile.c_str() << flush;
		    // glibc's __need protocol: a request macro (__need_size_t,
		    // __need_wchar_t, gcc's __need___va_list, ...) live at the
		    // include marks a PROTOCOL VISIT — the header is DESIGNED
		    // for repeated inclusion, serving one definition per pass
		    // and clearing the request. Such a visit must reach the
		    // protocol-aware TEXT: no name-level once-only skip (the
		    // first visit served a DIFFERENT request), no baked PCH,
		    // no forest bind (both are the full-content one-shot).
		    // And the freeze must form NO unit for it — the serving
		    // belongs to the INCLUDER (pack_protocol_serving_begin;
		    // both the embedded and the filesystem arm gate on this).
		    bool protocol_visit = need_protocol_macro_live();
		    // angle-bracket includes: prefer real precompiled headers, then
		    // text-embedded compatibility headers, then filesystem source.
		    // #include_next is POSITIONAL (continue the path search after
		    // the current header's dir) — it resolves through the named
		    // providers ONLY when the positional walk says the embedded
		    // set's slot is the next provider (embedded_wins_include_next:
		    // libc++'s C wrappers reaching the hosted-darwin prelude);
		    // otherwise it stays a pure filesystem walk.
		    if ( is_system
		      && (!is_include_next || embedded_wins_include_next(incfile)) )
		    {
			if ( !suppress_auto_include_scan && !protocol_visit
			  && pending_auto_include_headers.count(incfile) )
			{
			    // Defer to the auto-include prelude ONLY when a
			    // named provider (PCH / embedded) can actually
			    // supply the header — the prelude has no filesystem
			    // walk, so deferring an unprovided EXPLICIT include
			    // would drop it silently. Unprovided names fall
			    // through to the direct path: filesystem walk, loud
			    // open failure on a true miss (gcc canon: explicit
			    // includes resolve or error).
			    std::string defer_pch_path;
			    if ( named_include_provider_exists(*this, incfile,
							       defer_pch_path) )
			    {
				DBG(std::cout << "#include <" << incfile
				    << "> deferred to auto-include prelude" << std::endl);
				return getToken();
			    }
			}
			// Phase 6 (--forest-bind): a grove-backed system header
			// BINDS instead of tokenizing — install its PP-export
			// delta along the include DAG (forest_bind_include), then
			// restore the forest's typed decl records into the symbol
			// tables (forest_restore_decls, once per compile), and
			// return WITHOUT re-parsing the header. Non-forest headers
			// fall through to live parse. Gated on the policy knob
			// so the default path is one predicted branch.
			if ( registration_policy.enable_forest_bind
			  && !protocol_visit )
			{
			    int fu = forest_unit_for_include(incfile);
			    // A parent bind selected this exact child for live
			    // tokenization. Its recursive synthetic include must pass
			    // through the normal provider path, never re-bind the husk.
			    if ( fu >= 0
			      && forest_husk_live_pending.count((uint32_t)fu) )
				fu = -1;
			    // v40 (task #57): a unit whose frozen branch
			    // decisions assumed a different macro environment
			    // declines here and live-parses below.
			    if ( fu >= 0 && !forest_bind_env_ok((uint32_t)fu) )
				fu = -1;
			    if ( fu >= 0 )
			    {
				DBG(std::cout << "#include <" << incfile
				    << "> bound to grove unit " << fu << " ("
				    << bind_forest->unit_name(fu) << ")" << std::endl);
				forest_bind_include((uint32_t)fu);
				// v25: the embedded-header include FLAG is a live
				// tokenize-time side effect (_parser_init's lazy_map
				// registration: stdin/stdout/stderr, cout/cin). A
				// grove unit whose name IS the bare embedded-header
				// name was tokenized from the embedded text at
				// freeze — re-run the one live side effect. A
				// REAL-header unit (path name) never marks, exactly
				// like the live filesystem branch.
				const char *bound_unit_name = bind_forest->unit_name(fu);
				const bool bound_embedded = bound_unit_name
				    && find_embedded_header(bound_unit_name);
				if ( bound_embedded )
				    mark_embedded_include_flag(bound_unit_name);
				// Item 5 (lazy defrost): the decl-record restore
				// moved to flush_forest_pending_globals — the
				// post-tokenize point where forest_chain_set is
				// COMPLETE, so registration filters to the TU's
				// actual bound-include closure (live parity: a
				// header you never include declares nothing).
				bool bound_real_provider = bound_unit_name
				    && !bound_embedded
				    && is_system_header_path(bound_unit_name);
				if ( bound_unit_name && !bound_real_provider
				  && !bound_embedded )
				{
				    const std::string bound_name(bound_unit_name);
				    const std::string suffix = ".madh";
				    if ( bound_name.size() > suffix.size()
				      && bound_name.compare(bound_name.size() - suffix.size(),
						    suffix.size(), suffix) == 0 )
					bound_real_provider = is_system_header_path(
					    bound_name.substr(0, bound_name.size()
							      - suffix.size()).c_str());
				    else if ( bound_name == incfile
					   && find_precompiled_header(incfile) )
					bound_real_provider = true;
				}
				if ( bound_real_provider )
				    tokenize_posix_header_supplement(incfile);
				return include_completed_token();
			    }
			}
			// The NAME-level once-only key gates ONLY the named
			// PCH/embedded resolutions (their baked token sets assume
			// single inclusion and carry no #ifndef guards). A
			// filesystem-resolved header must NOT be deduped by name:
			// its dedup is the guard-aware full-path check below, so a
			// deliberately guard-less header (bits/mathcalls.h, multi-
			// included with a different _Mdouble_ per pass) re-tokenizes.
			std::string include_key = "<" + incfile + ">";
			bool name_already_included = !protocol_visit
			    && include_already_seen(include_key);
			std::string pch_path;
			bool resolves_named =
			    named_include_provider_exists(*this, incfile, pch_path);
			if ( protocol_visit )
			    pch_path.clear();	// a baked PCH is the full one-shot
			if ( resolves_named && name_already_included )
			{
			    // B4a: the include EDGE exists in the source even when
			    // the once-only dedup skips re-tokenization — the bind-
			    // time PP-export composition walks these edges.
			    pack_record_edge(pch_path.empty() ? incfile : pch_path);
			    DBG(std::cout << "#include <" << incfile << "> skipped (already included)" << std::endl);
			    return getToken();
			}
			if ( resolves_named && !protocol_visit )
			    should_tokenize_include(include_key);   // record the name
			// (a protocol visit records nothing: it served ONE
			// request, not the header — the full content is
			// still owed to a later plain include)
			if ( !pch_path.empty() )
			{
			    DBG(std::cout << "#include <" << incfile << "> (precompiled file "
				<< pch_path << ")" << std::endl);
			    std::deque<TokenBase *> pch_tokens;
			    if ( load_precompiled_header_file(pch_path, pch_tokens) )
			    {
				push_precompiled_header_tokens(*this, pch_path, pch_tokens);
				std::string pch_source_path = pch_path.substr(
				    0, pch_path.size() - sizeof(".madh") + 1);
				if ( is_system_header_path(pch_source_path.c_str()) )
				    tokenize_posix_header_supplement(incfile);
				return include_completed_token();
			    }
			    DBG(std::cout << "#include <" << incfile
				<< "> filesystem PCH failed, trying embedded PCH" << std::endl);
			}
			const PrecompiledHeader *pch = find_precompiled_header(incfile);
			if ( pch )
			{
			    DBG(std::cout << "#include <" << incfile << "> (precompiled)" << std::endl);
			    std::deque<TokenBase *> pch_tokens;
			    if ( madc_pch::read_madh(pch->data, pch->size, pch_tokens) )
			    {
				push_precompiled_header_tokens(*this, incfile, pch_tokens);
				tokenize_posix_header_supplement(incfile);
				return include_completed_token();
			    }
			    DBG(std::cout << "#include <" << incfile << "> PCH failed, trying embedded text" << std::endl);
			}
			const std::string *embedded = find_embedded_header(incfile);
			// Disallowed by policy (e.g. --no-embedded-headers): skip madc's
			// baked-in stub and fall through to the real filesystem header
			// below, rather than erroring. The host's include_paths still
			// gate what the filesystem search can reach.
			if ( embedded && !is_embedded_header_allowed(incfile) )
			{
			    DBG(std::cout << "#include <" << incfile
				<< "> embedded stub disallowed by policy; using filesystem" << std::endl);
			    embedded = NULL;
			}
			// Outranked by a directory ahead of madc's resource-dir
			// slot (a C++ standard library's own wrapper, or any -I
			// dir): the real header wins and we fall through to the
			// filesystem walk below. Shadowing libc++'s <stddef.h>
			// here is exactly what makes it #error that its wrapper
			// was bypassed. #include_next skips this from-the-TOP
			// test — its ranking was already decided positionally
			// by embedded_wins_include_next at the block gate.
			if ( embedded && !is_include_next
			  && embedded_header_outranked(incfile) )
			{
			    DBG(std::cout << "#include <" << incfile
				<< "> outranked by an earlier search dir; using the real header" << std::endl);
			    embedded = NULL;
			}
			if ( embedded )
			{
			    DBG(std::cout << "#include <" << incfile << "> (embedded)" << std::endl);
			    tokenize_embedded_header_text(incfile, *embedded,
							  protocol_visit);
			    return include_completed_token();
			}
		    }
		    std::string full_path = is_include_next
			? resolve_include_next_path(incfile)
			: resolve_include_path(incfile, is_system);
		    // A POSIX header the native toolchain does not ship AT ALL
		    // has no real provider to augment, so the posix/ entry is
		    // the provider itself. Decided here, after the walk, on its
		    // actual outcome rather than a predicted one.
		    if ( is_system && !is_include_next
		      && tokenize_posix_whole_provider(incfile, full_path) )
			return include_completed_token();
		    // Phase 6 (--forest-bind), v25: a grove-backed QUOTED /
		    // filesystem include BINDS instead of tokenizing, exactly like
		    // the angle branch above — the forest holds EVERY #include's
		    // state (v24 root-vs-include discriminator: user headers
		    // restore), so a live re-parse BESIDE the restored state would
		    // double-define its contents ("Repeated item declaration").
		    // Keyed on the RESOLVED path (the name the freeze tokenized
		    // the unit under). #include_next keeps its positional walk.
		    if ( registration_policy.enable_forest_bind && !is_include_next
		      && !full_path.empty() )
			{
			int fu = forest_unit_for_include(full_path);
			if ( fu >= 0
			  && forest_husk_live_pending.count((uint32_t)fu) )
			    fu = -1;
			// v40 (task #57): environment mismatch declines to
			// live parse.
			if ( fu >= 0 && !forest_bind_env_ok((uint32_t)fu) )
			    fu = -1;
			if ( fu >= 0 )
			{
			    DBG(std::cout << "#include \"" << full_path
				<< "\" bound to grove unit " << fu << " ("
				<< bind_forest->unit_name(fu) << ")" << std::endl);
			    forest_bind_include((uint32_t)fu);
			    if ( is_system
			      && is_system_header_path(full_path.c_str()) )
				tokenize_posix_header_supplement(incfile);
			    // Item 5: decl restore rides the post-tokenize
			    // flush (see the system-include bind site).
			    return include_completed_token();
			}
		    }
		    if ( !should_tokenize_include(full_path) )
		    {
			if ( !protocol_visit )	// a protocol visit records nothing
			{
			    pack_record_edge(full_path);	// B4a: edge survives the dedup skip
			    pack_record_skipped_source(full_path);	// ...and so must its CONTENT
			}
			DBG(std::cout << "#include "
			    << (is_system ? "<" : "\"") << full_path
			    << (is_system ? ">" : "\"")
			    << " skipped (already included)" << std::endl);
			return getToken();
		    }
		    DBG(std::cout << "#include "
			<< (is_system ? "<" : "\"") << full_path
			<< (is_system ? ">" : "\"") << std::endl);
		    // B4a: a protocol visit (a real gcc/glibc stddef.h reached
		    // through the filesystem walk, or a stdlib wrapper on the
		    // way to one — the request macro is still live at ITS
		    // include site) forms no unit and no edge; the serving
		    // belongs to the includer's unit (owner captured pre-swap).
		    const char *_proto_saved = NULL;
		    if ( protocol_visit )
			_proto_saved = pack_protocol_serving_begin();
		    else
			pack_record_edge(full_path);	// B4a: includer -> includee, pre-swap
		    // save current source, tokenize included file
		    Source saved = std::move(source);
		    bool saved_suppress_auto_include_scan = suppress_auto_include_scan;
		    // A SYSTEM header's internal identifiers must never trigger
		    // the auto-include convenience scan. A QUOTED include of the
		    // user's own file is the user's dialect code — bare
		    // print/php::/value identifiers there get the same
		    // auto-include service the main file gets (suppressing both
		    // is the residue that forced advent.mad to spell out its
		    // includes). A quoted include that resolves INTO a system
		    // path stays suppressed — classify by the resolved path,
		    // not the spelling. User units are also RECORDED: the
		    // auto-include prelude must insert before the first
		    // user-code token, module or main.
		    if ( is_system || is_system_header_path(full_path.c_str()) )
			suppress_auto_include_scan = true;
		    else
			auto_include_user_units.insert(intern_file(full_path));
		    source = Source();
		    // ONE owner for "this resolved include's bytes" (disk,
		    // then the forest's raw-source slot) — the same reader
		    // detect_include_guard() uses, so the tokenizer and the
		    // multiple-include optimization can never disagree about
		    // which headers are readable.
		    std::string include_text;
		    if ( !read_resolved_include(full_path, include_text) )
		    {
			suppress_auto_include_scan = saved_suppress_auto_include_scan;
			source = std::move(saved); // restore before throwing
			if ( protocol_visit )
			    pack_protocol_serving_end(_proto_saved);
			Throw << "Failed to open include file: " << full_path.c_str() << flush;
		    }
		    source.fname(full_path.c_str());
		    const char *_interned2 = intern_file(full_path);
		    {
			ReadTimer _rt(_read_seconds);
			_input_bytes += include_text.size();	// --show-stats: header bytes
			source.str(include_text);
		    }
		    TokenBase *itb;
		    if ( !protocol_visit )
		    {
			pack_note_unit(_interned2);
			pack_record_source(_interned2, source.text());
		    }
		    // v40 (task #57): unit-stack tracking — every unit entered
		    // beneath a stack member joins that member's SUBTREE set,
		    // the "reproduced by this unit's own bind replay" domain
		    // the branch-dep recorder tests against. Protocol visits
		    // form no unit and never touch the stack.
		    if ( pack_recording && !protocol_visit )
		    {
			for ( size_t si = 0; si < pack_unit_stack.size(); ++si )
			    pack_unit_subtree[pack_unit_stack[si]].insert(_interned2);
			pack_unit_subtree[_interned2].insert(_interned2);
			pack_unit_stack.push_back(_interned2);
		    }
		    while ( (itb = getRealToken()) )
		    {
			itb->file = _interned2;
			push_token_with_literal_concat(itb);
		    }
		    if ( pack_recording && !protocol_visit )
			pack_unit_stack.pop_back();
		    source = std::move(saved);
		    suppress_auto_include_scan = saved_suppress_auto_include_scan;
		    if ( protocol_visit )
			pack_protocol_serving_end(_proto_saved);
		    // A supplement augments the REAL system header after its tokens
		    // and macros have been served. It is injected from the restored
		    // includer so forest edges retain source order: real, then delta.
		    if ( is_system && !protocol_visit
		      && is_system_header_path(full_path.c_str()) )
			tokenize_posix_header_supplement(incfile);
		    return include_completed_token(); // continue with current file
		}
		if ( directive == "load" )
		{
		    // #load "libfoo.so" as namespace;
		    if ( !is_dynamic_library_loading_enabled() )
			Throw << "#load is disabled by registration policy" << flush;
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    char delim = source.get(); // "
		    std::string libname;
		    while ( source.good() && !source.eof() && source.peek() != delim
		    &&      source.peek() != '\n' && source.peek() != '\r' )
			libname += source.get();
		    if ( source.peek() == delim )
			source.get();
		    // skip whitespace, expect "as"
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string kw;
		    while ( source.good() && !source.eof() && isalpha(source.peek()) )
			kw += source.get();
		    if ( kw != "as" )
			throw "Expecting 'as' in #load directive";
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string ns_name;
		    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
			ns_name += source.get();
		    // skip to semicolon
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    if ( source.peek() == ';' )
			source.get();
		    // dlopen the library — unless auto-library-loading is off, in
		    // which case the named library is NOT loaded; the namespace is
		    // bound to the global symbol scope (dlopen(NULL)) so its symbols
		    // come from explicit linking (`madc -l<lib>` / the host) instead.
		    void *handle;
		    if ( is_auto_library_loading_enabled() )
		    {
			handle = madcdl_open_global(libname.c_str());
			if ( !handle )
			{
			    std::string err = "Failed to load library: " + libname + ": " + madcdl_error();
			    Throw << err.c_str() << flush;
			}
			DBG(std::cout << "#load \"" << libname << "\" as " << ns_name << std::endl);
			loaded_lib_paths.push_back(libname);
		    }
		    else
		    {
			handle = madcdl_open_self();
			DBG(std::cout << "#load \"" << libname << "\" as " << ns_name
				      << " — auto-load off, bound to global scope" << std::endl);
		    }
		    dlopen_map[ns_name] = handle;
		    namespace_variables_for_write(ns_name); // create empty namespace
		    return getToken();
		}
		if ( directive == "define" )
		{
		    // #define NAME[(params)] value
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string name;
		    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
			name += source.get();

		    // function-like macro: ( immediately after name (no space)
		    if ( source.peek() == '(' )
		    {
			source.get(); // consume '('
			MacroDef macro;
			// read parameter names
			while ( source.good() && source.peek() != ')' )
			{
			    while ( source.peek() == ' ' || source.peek() == '\t' || source.peek() == ',' )
				source.get();
			    if ( source.peek() == ')' ) break;
			    if ( source.peek() == '.' )
			    {
				source.get();
				if ( source.peek() != '.' )
				    Throw << "Expecting '...' in variadic macro parameter list" << flush;
				source.get();
				if ( source.peek() != '.' )
				    Throw << "Expecting '...' in variadic macro parameter list" << flush;
				source.get();
				macro.variadic = true;
				while ( source.peek() == ' ' || source.peek() == '\t' )
				    source.get();
				if ( source.peek() != ')' )
				    Throw << "Variadic macro '...' must be the last parameter" << flush;
				break;
			    }
			    std::string param;
			    while ( source.good() && (isalnum(source.peek()) || source.peek() == '_') )
				param += source.get();
			    if ( param.empty() )
				Throw << "Expecting macro parameter name" << flush;
			    if ( source.peek() == '.' )
			    {
				source.get();
				if ( source.peek() != '.' )
				    Throw << "Expecting '...' after named variadic macro parameter" << flush;
				source.get();
				if ( source.peek() != '.' )
				    Throw << "Expecting '...' after named variadic macro parameter" << flush;
				source.get();
				macro.variadic = true;
				macro.variadic_param = param;
				macro.params.push_back(param);
				while ( source.peek() == ' ' || source.peek() == '\t' )
				    source.get();
				if ( source.peek() != ')' )
				    Throw << "Variadic macro '...' must be the last parameter" << flush;
				break;
			    }
			    macro.params.push_back(param);
			}
			if ( source.peek() == ')' ) source.get(); // consume ')'
			// skip whitespace before body
			while ( source.peek() == ' ' || source.peek() == '\t' )
			    source.get();
			std::string body = read_macro_body(source);
			macro.body = body;
			macro_map[name] = macro;
			pack_record_define_fn(name, macro);
			DBG(std::cout << "#define " << name << "(");
			DBG(for (size_t i=0; i<macro.params.size(); ++i) { if (i) std::cout << ","; std::cout << macro.params[i]; });
			DBG(std::cout << ") " << body << std::endl);
			return getToken();
		    }

		    // object-like macro: #define NAME value
		    // skip whitespace between name and value
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string value = read_macro_body(source);
		    define_map[name] = value;
		    note_std_abi_define(name, value);
		    pack_record_define(name, value);
		    DBG(std::cout << "#define " << name << " " << value << std::endl);
		    return getToken();
		}
		if ( directive == "undef" )
		{
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string name;
		    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
			name += source.get();
		    define_map.erase(name);
		    macro_map.erase(name);
		    pack_record_undef(name);
		    DBG(std::cout << "#undef " << name << std::endl);
		    // discard the directive's trailing tokens via the lexer, so a
		    // multi-line /* */ comment here is handled by the lexer's case '/'
		    consume_directive_line_tail();
		    return getToken();
		}
		if ( directive == "line" )
		{
		    // C11 6.10.4: `#line N ["file"]` — the digit sequence
		    // (after macro expansion, p5: `#line line` is legal) sets
		    // the FOLLOWING line's presumed number; the optional
		    // string renames the presumed file. The old handler
		    // discarded the directive entirely, so __LINE__ and
		    // diagnostics kept physical numbering (c-testsuite 00152).
		    std::string raw;
		    while ( source.good() && !source.eof()
			 && source.peek() != '\n' && source.peek() != '\r' )
			raw += source.get();
		    std::string arg = expandIfMacros(raw);
		    size_t p = 0;
		    while ( p < arg.size() && (arg[p] == ' ' || arg[p] == '\t') )
			++p;
		    long lineno = 0;
		    bool have_digits = false;
		    while ( p < arg.size() && isdigit((unsigned char)arg[p]) )
		    {
			lineno = lineno * 10 + (arg[p] - '0');
			++p;
			have_digits = true;
		    }
		    while ( p < arg.size() && (arg[p] == ' ' || arg[p] == '\t') )
			++p;
		    std::string newname;
		    bool have_name = false;
		    if ( p < arg.size() && arg[p] == '"' )
		    {
			++p;
			while ( p < arg.size() && arg[p] != '"' )
			    newname += arg[p++];
			have_name = true;
		    }
		    // Consume the terminator here so the renumbering starts
		    // exactly at the next physical line.
		    if ( source.peek() == '\r' )
			source.get();
		    if ( source.peek() == '\n' )
			source.get();
		    if ( have_digits )
			source.setpos((int)lineno, 0);
		    if ( have_name )
			source.fname(newname.c_str());
		    return getToken();
		}
		if ( directive == "ifdef" || directive == "ifndef" )
		{
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string name;
		    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
			name += source.get();
		    bool defined = macro_name_defined(name);
		    pack_record_branch_macro(name);
		    bool active = (directive == "ifdef") ? defined : !defined;
		    ifdef_stack.push(active);
		    ifdef_done_stack.push(active);
		    DBG(std::cout << "#" << directive << " " << name << " -> " << (active ? "true" : "false") << " stack=" << ifdef_stack.size() << " file=" << source.fname() << std::endl);
		    // discard the directive's trailing tokens via the lexer, so a
		    // multi-line /* */ comment here is handled by the lexer's case '/'
		    consume_directive_line_tail();
		    if ( !active )
			return skipConditionalBlock();
		    return getToken();
		}
		if ( directive == "if" )
		{
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    bool active = evaluateIfCondition();
		    ifdef_stack.push(active);
		    ifdef_done_stack.push(active);
		    DBG(std::cout << "#if -> " << (active ? "true" : "false") << std::endl);
		    if ( !active )
			return skipConditionalBlock();
		    return getToken();
		}
		if ( directive == "elif" )
		{
		    if ( ifdef_stack.empty() )
			Throw << "#elif without matching #if/#ifdef" << flush;
		    bool already_done = ifdef_done_stack.top();
		    ifdef_stack.pop();
		    if ( already_done )
		    {
			ifdef_stack.push(false);
			return skipConditionalBlock();
		    }
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    bool active = evaluateIfCondition();
		    ifdef_stack.push(active);
		    if ( active )
			ifdef_done_stack.top() = true;
		    DBG(std::cout << "#elif -> " << (active ? "true" : "false") << std::endl);
		    if ( !active )
			return skipConditionalBlock();
		    return getToken();
		}
		if ( directive == "else" )
		{
		    if ( ifdef_stack.empty() )
			Throw << "#else without matching #if/#ifdef" << flush;
		    bool already_done = ifdef_done_stack.top();
		    ifdef_stack.pop();
		    bool active = !already_done;
		    ifdef_stack.push(active);
		    if ( active )
			ifdef_done_stack.top() = true;
		    DBG(std::cout << "#else -> " << (active ? "true" : "false") << " stack=" << ifdef_stack.size() << " file=" << source.fname() << std::endl);
		    // discard the directive's trailing tokens via the lexer, so a
		    // multi-line /* */ comment here is handled by the lexer's case '/'
		    consume_directive_line_tail();
		    if ( !active )
			return skipConditionalBlock();
		    return getToken();
		}
		if ( directive == "endif" )
		{
		    if ( ifdef_stack.empty() )
			Throw << "#endif without matching #if/#ifdef" << flush;
		    ifdef_stack.pop();
		    ifdef_done_stack.pop();
		    DBG(std::cout << "#endif" << std::endl);
		    // discard the directive's trailing tokens via the lexer, so a
		    // multi-line /* */ comment here is handled by the lexer's case '/'
		    consume_directive_line_tail();
		    return getToken();
		}
		if ( directive == "error" || directive == "warning" )
		{
		    // #error message — emit compile error with the message text
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string msg;
		    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			msg += source.get();
		    if ( directive == "error" )
			Throw << "#error " << msg << flush;
		    // #warning is just a diagnostic, skip it
		    return getToken();
		}
		if ( directive == "pragma" )
		{
		    handle_pragma_body();
		    return _getToken();
		}
	    }
	    DBG(std::cout << "# fell through to TokenHash, peek=" << (int)source.peek() << " file=" << source.fname() << std::endl);
	    return make_token(TokenID::tkHash);
	case '{': return make_token(TokenID::tkOpBrc);
	case '}': return make_token(TokenID::tkClBrc);
	case '(': return make_token(TokenID::tkOpBrk);
	case ')': return make_token(TokenID::tkClBrk);
	case '[':
	    // C23 [[attribute]] syntax: consume [[...]] and skip
	    if ( source.peek() == '[' )
	    {
		source.get(); // consume second '['
		// Skip until matching ]]
		while ( source.good() )
		{
		    char c = source.get();
		    if ( c == ']' && source.peek() == ']' )
		    {
			source.get(); // consume second ']'
			break;
		    }
		}
		return getToken();
	    }
	    return make_token(TokenID::tkOpSqr);
	case ']': return make_token(TokenID::tkClSqr);
	case '~': return make_token(TokenID::tkBnot);
	case '!': if (source.peek() != '=') return make_token(TokenID::tkLnot);		// !
	    source.get();
	    // !== is the madc dialect's strict not-equal; below the floor it
	    // lexes as != then = (conforming syntax error).
	    if (source.peek() == '=' && language_std == STD_MADC)
		{ source.get(); return make_token(TokenID::tk3NotEq); }		// !==
	    return make_token(TokenID::tkNotEq);					// !=
	case '&':
	    if (source.peek() == '&') { source.get(); return make_token(TokenID::tkLand);   }  // &&
	    if (source.peek() == '=') { source.get(); return make_token(TokenID::tkBandEq); }  // &=
	    return make_token(TokenID::tkBand);					// &
	case '|':
	    if (source.peek() == '|') { source.get(); return make_token(TokenID::tkLor);    }  // ||
	    if (source.peek() == '=') { source.get(); return make_token(TokenID::tkBorEq);  }  // |=
	    return make_token(TokenID::tkBor);					// |
	case '%': if (source.peek() != '=') return make_token(TokenID::tkMod);		// %
	    source.get(); return make_token(TokenID::tkModEq);				// %=
	case '^': if (source.peek() != '=') return make_token(TokenID::tkXor);		// ^
	     source.get(); return make_token(TokenID::tkXorEq);				// ^=
	case '?': return make_token(TokenID::tkQmark);					// ?
	case ':':
	    if (source.peek() == ':') { source.get(); return make_token(TokenID::tkNS); }   // ::
	    if (source.peek() == '=') { source.get(); return make_token(TokenID::tkColEq); } // :=
	    return make_token(TokenID::tkColon);                                               // :
	case ';': return make_token(TokenID::tkSemi);					// ,
	case ',': return make_token(TokenID::tkComma);				// .
	case '.':
	    // Leading-dot float literal: `.4`, `.25f`, etc. are valid C
	    // shorthand for `0.4`, `0.25f`. The lexer used to tokenize
	    // `.4` as TokenDot followed by integer, which the expression
	    // parser rejected as `Missing operand`.
	    if ( source.good() && isdigit(source.peek()) )
	    {
		double num = 0, divisor = 10;
		while ( source.good() && isdigit(source.peek()) )
		{
		    num += (source.get() & 0xf) / divisor;
		    divisor *= 10;
		}
		// exponent
		if ( source.good() && (source.peek() == 'e' || source.peek() == 'E') )
		{
		    source.get();
		    int esign = 1;
		    if ( source.good() && (source.peek() == '+' || source.peek() == '-') )
		    {
			if ( source.peek() == '-' ) esign = -1;
			source.get();
		    }
		    int ev = 0;
		    while ( source.good() && isdigit(source.peek()) )
			ev = ev * 10 + (source.get() & 0xf);
		    double f = 1.0;
		    for ( int i = 0; i < ev; ++i ) f *= 10.0;
		    if ( esign > 0 ) num *= f; else num /= f;
		}
		if ( source.good() )
		{
		    int c = source.peek();
		    if ( c == 'f' || c == 'F' || c == 'l' || c == 'L' )
			source.get();
		}
		return make_real(num);
	    }
	    return make_token(TokenID::tkDot);
	case '"':
	    word = "";
	    row = source.line();
	    col = source.column();
	    while ( source.good() && source.peek() != '"' )
	    {
		if ( source.peek() == '\\' )
		{
		    source.get(); // consume backslash
		    if ( !source.good() ) break;
		    char esc = source.get();
		    switch (esc) {
			case 'n':  word += '\n'; break;
			case 't':  word += '\t'; break;
			case 'r':  word += '\r'; break;
			case '\\': word += '\\'; break;
			case '"':  word += '"';  break;
			case '\'': word += '\''; break;
			case 'a':  word += '\a'; break;
			case 'b':  word += '\b'; break;
			case 'f':  word += '\f'; break;
			case 'v':  word += '\v'; break;
			case '?':  word += '\?'; break;
			case 'x': case 'X': {
			    // hex escape: \xHH (1-2 hex digits)
			    int val = 0; int dig = 0;
			    while ( dig < 2 && source.good() ) {
				int c = source.peek();
				int d = (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1;
				if ( d < 0 ) break;
				val = (val << 4) | d;
				source.get(); ++dig;
			    }
			    word += (char)val;
			    break;
			}
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7': {
			    // octal escape: \NNN (1-3 octal digits, including the one already consumed)
			    int val = esc - '0'; int dig = 1;
			    while ( dig < 3 && source.good() ) {
				int c = source.peek();
				if ( c < '0' || c > '7' ) break;
				val = (val << 3) | (c - '0');
				source.get(); ++dig;
			    }
			    word += (char)val;
			    break;
			}
			default:   word += '\\'; word += esc; break;
		    }
		}
		else
		    word += source.get();
	    }
	    if ( !source.good() )
	    {
		source.setpos(row, col);
		Throw << "Unterminated string" << flush;
	    }
	    source.get();
	    {
		TokenBase *stok = make_str(word);
		// Source extent of this piece (see TokenStr::SrcPiece): from
		// the opening quote through the closing quote, single-line
		// only — a line-spanning literal (scanner-tolerated) keeps no
		// piece and the consumer falls back.
		if ( source.line() == row )
		{
		    TokenStr::SrcPiece pc;
		    pc.line = row;
		    pc.col = col - 1;
		    pc.len = source.column() - pc.col;
		    ((TokenStr *)stok)->src_pieces.push_back(pc);
		}
		return stok;
	    }
	case '\'':
	    word = "";
	    row = source.line();
	    col = source.column();
	    while ( source.good() && source.peek() != '\'' )
	    {
		if ( source.peek() == '\\' )
		{
		    source.get(); // consume backslash
		    if ( !source.good() ) break;
		    char esc = source.get();
		    switch (esc) {
			case 'n':  word += '\n'; break;
			case 't':  word += '\t'; break;
			case 'r':  word += '\r'; break;
			case '\\': word += '\\'; break;
			case '\'': word += '\''; break;
			case '"':  word += '"';  break;
			case 'a':  word += '\a'; break;
			case 'b':  word += '\b'; break;
			case 'f':  word += '\f'; break;
			case 'v':  word += '\v'; break;
			case '?':  word += '\?'; break;
			case 'x': case 'X': {
			    int val = 0; int dig = 0;
			    while ( dig < 2 && source.good() ) {
				int c = source.peek();
				int d = (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1;
				if ( d < 0 ) break;
				val = (val << 4) | d;
				source.get(); ++dig;
			    }
			    word += (char)val;
			    break;
			}
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7': {
			    int val = esc - '0'; int dig = 1;
			    while ( dig < 3 && source.good() ) {
				int c = source.peek();
				if ( c < '0' || c > '7' ) break;
				val = (val << 3) | (c - '0');
				source.get(); ++dig;
			    }
			    word += (char)val;
			    break;
			}
			default:   word += '\\'; word += esc; break;
		    }
		    continue;
		}
		word += source.get();
	    }
	    if ( !source.good() )
	    {
		source.setpos(row, col);
		Throw << "Unterminated string" << flush;
	    }
	    source.get();
	    return make_char(word[0]);
	case '<':
	    if (source.peek() == '=')
	    {
		source.get();
		// <=> is C++20 (and the madc dialect). Below the std floor the
		// sequence lexes as <= then > — pre-C++20 sources that happen
		// to contain the characters keep their old meaning.
		if (source.peek() == '>'
		 && (language_std == STD_MADC || language_std >= STD_CPP20))
		    { source.get(); return make_token(TokenID::tk3Way); }		  // <=>
		return make_token(TokenID::tkLE);					  // <=
	    }
	    if (source.peek() == '<')
	    {
		source.get();
		if (source.peek() == '=') { source.get(); return make_token(TokenID::tkBSLEq); } // <<=
		return make_token(TokenID::tkBSL);					  // <<
	    }
	    return make_token(TokenID::tkLT);						  // <
	case '>':
	    if (source.peek() == '=')     { source.get(); return make_token(TokenID::tkGE);  }	  // >=
	    if (source.peek() == '>')
	    {
		source.get();
		if (source.peek() == '=') { source.get(); return make_token(TokenID::tkBSREq); } // >>=
		return make_token(TokenID::tkBSR);					  // >>
	    }
	    return make_token(TokenID::tkGT);						  // >
	default:
	    if ( isdigit(ch) )
	    {
		auto complex_real_type_for_suffix = [&](char suffix) -> DataDef * {
		    if ( suffix == 'f' || suffix == 'F' )
			return &ddFLOAT;
		    return &ddDOUBLE;
		};
		char imag_type_suffix = 0;
		auto eat_imag_suffix = [&]() {
		    imag_type_suffix = 0;
		    if ( !source.good() )
			return false;
		    int c = source.peek();
		    if ( c != 'i' && c != 'I' && c != 'j' && c != 'J' )
			return false;
		    source.get();
		    if ( source.good() )
		    {
			int tc = source.peek();
			if ( tc == 'f' || tc == 'F' || tc == 'l' || tc == 'L' )
			{
			    imag_type_suffix = (char)tc;
			    source.get();
			}
		    }
		    return true;
		};
		// ONE eater for the real-literal type suffixes, shared by the
		// hex-float, scientific and decimal paths (three hand-rolled
		// copies would drift): classic f/F -> float and l/L -> long
		// double, the DFP d/dd spellings (eaten, type left alone —
		// the pre-existing posture), and the C23 _FloatN family
		// f16/f32/f64/f128 plus gcc's bf16, which ride the SAME
		// nearest-supported approximation as the _Float16/_Float32
		// TYPE spellings (add_datatypes): f16/f32/bf16 -> float,
		// f64 -> double, f128 -> long double. mingw's
		// avx512fp16intrin.h spells `0.0f16`; the old one-char
		// eaters left `16` behind as a second operand (loud since
		// the s149b juxtaposition wall — the win release pack's
		// ledger build of rt_posix_time.c was the first to hit it).
		// A width that is not a _FloatN width is pushed back WHOLE,
		// so invalid spellings keep today's two-token diagnosis; a
		// _FloatN imaginary keeps the plain f-suffix complex-float
		// posture (the eat_imag_suffix call sites are unchanged).
		char real_type_suffix = 0;
		DataDef *real_suffix_dd = nullptr;
		auto eat_real_suffix = [&](std::string &lt) {
		    real_type_suffix = 0;
		    real_suffix_dd = nullptr;
		    if ( !source.good() )
			return;
		    int c = source.peek();
		    if ( c == 'b' || c == 'B' )
		    {
			// bf16/BF16 only; 'b' alone never starts a suffix —
			// restore every consumed char on a mismatch.
			std::string got;
			got += (char)source.get();
			for ( const char *want = "f16"; *want && source.good(); want++ )
			{
			    if ( tolower(source.peek()) != *want )
				break;
			    got += (char)source.get();
			}
			if ( got.size() == 4 )
			{
			    lt += got;
			    real_type_suffix = 'f';
			    real_suffix_dd = &ddFLOAT;
			}
			else
			    source.pushback(got);
			return;
		    }
		    if ( c == 'f' || c == 'F' )
		    {
			real_type_suffix = (char)c;
			lt += (char)source.get();
			if ( source.good() && isdigit(source.peek()) )
			{
			    std::string w;
			    while ( source.good() && isdigit(source.peek())
				 && w.size() < 3 )
				w += (char)source.get();
			    if ( w == "16" || w == "32" )
				real_suffix_dd = &ddFLOAT;
			    else if ( w == "64" )
				real_suffix_dd = &ddDOUBLE;
			    else if ( w == "128" )
				real_suffix_dd = &ddLDOUBLE;
			    if ( real_suffix_dd )
				lt += w;
			    else
				source.pushback(w);
			}
			return;
		    }
		    if ( c == 'l' || c == 'L' )
		    {
			real_type_suffix = (char)c;
			lt += (char)source.get();
			return;
		    }
		    if ( c == 'd' || c == 'D' )
		    {
			lt += (char)source.get();
			if ( source.good() && (source.peek() == 'd' || source.peek() == 'D') )
			    lt += (char)source.get();
			return;
		    }
		};
		// Consume C integer-literal suffixes (u/U, l/L, ll/LL, combos
		// like ul/ULL/Lu) and set type accordingly. With sizeof(int)=4,
		// the L/LL suffix widens to 64-bit (long/long long), and U
		// controls signedness.
		bool has_u_suffix = false;
		int  long_count   = 0;
		std::string lit_text;
		lit_text += ch;
		auto eat_int_suffix = [&]() {
		    for (int i = 0; i < 3; ++i)
		    {
			int c = source.peek();
			if (c == 'u' || c == 'U')
			{
			    has_u_suffix = true;
			    lit_text += (char)source.get();
			}
			else if (c == 'l' || c == 'L')
			{
			    ++long_count;
			    lit_text += (char)source.get();
			}
			else
			    break;
		    }
		};
		auto resolve_int_suffix_type = [&](int64_t val, bool is_hex_or_octal) -> DataDef * {
		    if ( long_count >= 2 )
			return has_u_suffix ? (DataDef *)&ddUINT64 : (DataDef *)&ddINT64;
		    if ( long_count == 1 )
		    {
			// A single L means platform `long` — 8-byte on LP64
			// (dd_platform_long() IS ddINT64 there, unchanged), 4-byte
			// on LLP64, where C11 6.4.4.1 ladders a non-fitting value
			// past it: decimal L -> long, long long; hex/octal L adds
			// the unsigned rungs.
			if ( !target_llp64() )
			    return has_u_suffix ? (DataDef *)&ddUINT64
					        : (DataDef *)&ddINT64;
			uint64_t uval = (uint64_t)val;
			if ( has_u_suffix )
			    return uval <= 0xFFFFFFFFull ? dd_platform_ulong()
							 : (DataDef *)&ddUINT64;
			if ( uval <= 0x7FFFFFFFull )
			    return dd_platform_long();
			if ( is_hex_or_octal && uval <= 0xFFFFFFFFull )
			    return dd_platform_ulong();
			if ( (int64_t)uval >= 0 )
			    return &ddINT64;
			return is_hex_or_octal ? (DataDef *)&ddUINT64
					       : (DataDef *)&ddINT64;
		    }
		    // Bare U ladders past unsigned int when the value doesn't
		    // fit 32 bits (C11 6.4.4.1: unsigned int -> unsigned long
		    // -> unsigned long long; both 64-bit rungs are ddUINT64).
		    // gcc == clang == mingw-gcc: sizeof(4294967296U) is 8.
		    if ( has_u_suffix )
			return (uint64_t)val <= 0xFFFFFFFFull
			     ? (DataDef *)&ddUINT32 : (DataDef *)&ddUINT64;
		    // C integer literal type rules (no suffix):
		    // Hex/octal: int → unsigned int → long → unsigned long
		    // Decimal:   int → long → long long (never unsigned)
		    // (the >32-bit rungs land on ddINT64/ddUINT64 either way —
		    // on LLP64 the 4-byte long rung is just skipped, same as gcc)
		    uint64_t uval = (uint64_t)val;
		    if ( uval <= 0x7FFFFFFF )
			return nullptr; // fits in int32 — use default
		    if ( is_hex_or_octal && uval <= 0xFFFFFFFF )
			return &ddUINT32;
		    if ( (int64_t)uval >= 0 )
			return &ddINT64;
		    return is_hex_or_octal ? (DataDef *)&ddUINT64 : (DataDef *)&ddINT64;
		};
		// gcc canon (verified tmp/wide_lit.c + wide_lit2.c, host gcc): an
		// integer literal wider than 64 bits gets "integer constant is
		// too large for its type" and TRUNCATES to its low 64 bits — gcc
		// has no 128-bit literals (__int128 constants are composed as
		// (hi<<64)|lo), and the literal's TYPE is chosen from the
		// TRUNCATED value by the normal rules. madc matches that
		// behavior but keeps the full 128-bit value in Program::valpool
		// (TokenInt::wide_handle) so the token retains fidelity.
		auto finish_int_literal = [&](unsigned __int128 uval, bool ovf128,
					      bool is_hex_or_octal) -> TokenInt * {
		    int64_t tval = (int64_t)(uint64_t)uval;
		    TokenInt *ti = (TokenInt *)make_int(tval);
		    if ( ovf128 || (uval >> 64) != 0 )
		    {
			report_warning(DiagnosticPhase::lexer,
				       "integer constant is too large for its type",
				       source.fname(), source.line(), source.column());
			print_last_diagnostic(error());
			ti->wide_handle = valpool.put_u128((uint64_t)uval,
							   (uint64_t)(uval >> 64));
		    }
		    DataDef *st = resolve_int_suffix_type(tval, is_hex_or_octal);
		    if ( st )
			ti->setDataType(st);
		    return ti;
		};
		if ( is_binary_prefix(ch, source) )
		{
		    bool bv_ovf = false;
		    unsigned __int128 bv = read_binary_literal(source, bv_ovf);
		    eat_int_suffix();
		    // binary prefix source_text not critical for macro round-trip
		    return finish_int_literal(bv, bv_ovf, true);
		}
		// hex literal: 0x... or 0X...
		if ( ch == '0' && source.good() && (source.peek() == 'x' || source.peek() == 'X') )
		{
		    lit_text += (char)source.get(); // eat 'x'/'X'
		    unsigned __int128 hv = 0;
		    bool hv_ovf = false;
		    while ( source.good() )
		    {
			if ( is_digit_separator(source.peek()) )
			{
			    source.get();
			    continue;
			}
			if ( !isxdigit(source.peek()) )
			    break;
			char hc = source.get();
			lit_text += hc;
			if ( (hv >> 124) != 0 )
			    hv_ovf = true;	// next *16 shifts past 128 bits
			hv *= 16;
			if      ( hc >= '0' && hc <= '9' ) hv += hc - '0';
			else if ( hc >= 'a' && hc <= 'f' ) hv += hc - 'a' + 10;
			else                               hv += hc - 'A' + 10;
		    }
		    if ( source.good() && (source.peek() == '.'
		      || source.peek() == 'p' || source.peek() == 'P') )
		    {
			if ( source.peek() == '.' )
			{
			    lit_text += (char)source.get();
			    while ( source.good() )
			    {
				if ( is_digit_separator(source.peek()) )
				{
				    source.get();
				    continue;
				}
				if ( !isxdigit(source.peek()) )
				    break;
				lit_text += (char)source.get();
			    }
			}
			if ( source.good() && (source.peek() == 'p' || source.peek() == 'P') )
			{
			    lit_text += (char)source.get();
			    if ( source.good() && (source.peek() == '+' || source.peek() == '-') )
				lit_text += (char)source.get();
			    while ( source.good() && isdigit(source.peek()) )
				lit_text += (char)source.get();
			}
		    eat_real_suffix(lit_text);
		    if ( eat_imag_suffix() )
		    {
			TokenReal *tr = (TokenReal *)make_real(strtold(lit_text.c_str(), NULL));
			char suffix = imag_type_suffix ? imag_type_suffix : real_type_suffix;
			tr->setDataType(get_complex_compat_type(complex_real_type_for_suffix(suffix)));
			// Preserve full literal text with imaginary suffix for transpiler
			std::string full = lit_text;
			if (imag_type_suffix) { full += 'i'; full += imag_type_suffix; }
			else full += 'i';
			tr->source_text = full;
			return tr;
		    }
		    {
			TokenReal *tr = (TokenReal *)make_real(strtold(lit_text.c_str(), NULL));
			if ( real_suffix_dd )
			    tr->setDataType(real_suffix_dd);
			else if ( real_type_suffix == 'f' || real_type_suffix == 'F' )
			    tr->setDataType(&ddFLOAT);
			else if ( real_type_suffix == 'l' || real_type_suffix == 'L' )
			    tr->setDataType(&ddLDOUBLE);
			tr->source_text = lit_text;
			return tr;
		    }
		    }
		    eat_int_suffix();
		    {
			TokenInt *ti = finish_int_literal(hv, hv_ovf, true);
			ti->source_text = lit_text;
			return ti;
		    }
		}
		// Octal literal: starts with 0, digits 0-7
		bool is_octal = (ch == '0') && source.good()
		    && source.peek() >= '0' && source.peek() <= '7';
		unsigned __int128 v = (unsigned __int128)(ch & 0xf);
		bool v_ovf = false;

		if ( is_octal )
		{
		    while ( source.good() )
		    {
			if ( is_digit_separator(source.peek()) )
			    { source.get(); continue; }
			if ( source.peek() < '0' || source.peek() > '7' )
			    break;
			char oc = source.get();
			lit_text += oc;
			if ( (v >> 125) != 0 )
			    v_ovf = true;	// next *8 shifts past 128 bits
			v = v * 8 + (oc & 0xf);
		    }
		}
		else
		{
		    // 2^128-1 = ...211455: a next digit overflows iff v exceeds
		    // the /10 limit, or equals it with a digit above the final 5.
		    const unsigned __int128 dec_limit = (~(unsigned __int128)0) / 10;
		    while ( source.good() )
		    {
			if ( is_digit_separator(source.peek()) )
			{
			    source.get();
			    continue;
			}
			if ( !isdigit(source.peek()) )
			    break;
			char dc = source.get();
			lit_text += dc;
			if ( v > dec_limit || (v == dec_limit && (dc & 0xf) > 5) )
			    v_ovf = true;
			v *= 10;
			v += dc & 0xf;
		    }
		}
		// no decimal means integer — unless followed by e/E (scientific)
		if ( source.peek() != '.' )
		{
		    if ( source.peek() == 'e' || source.peek() == 'E' )
		    {
			// Scientific notation without decimal: 1e5, 2E-3
			lit_text += (char)source.get(); // consume e/E
			if ( source.good() && (source.peek() == '+' || source.peek() == '-') )
			    lit_text += (char)source.get();
			while ( source.good() && isdigit(source.peek()) )
			    lit_text += (char)source.get();
			eat_real_suffix(lit_text);
			long double num = strtold(lit_text.c_str(), NULL);
			if ( eat_imag_suffix() )
			{
			    TokenReal *tr = (TokenReal *)make_real(num);
			    char suffix = imag_type_suffix ? imag_type_suffix : real_type_suffix;
			    tr->setDataType(get_complex_compat_type(complex_real_type_for_suffix(suffix)));
			    std::string full = lit_text;
			    if (imag_type_suffix) { full += 'i'; full += imag_type_suffix; }
			    else full += 'i';
			    tr->source_text = full;
			    return tr;
			}
			TokenReal *tr = (TokenReal *)make_real(num);
			if ( real_suffix_dd )
			    tr->setDataType(real_suffix_dd);
			else if ( real_type_suffix == 'f' || real_type_suffix == 'F' )
			    tr->setDataType(&ddFLOAT);
			else if ( real_type_suffix == 'l' || real_type_suffix == 'L' )
			    tr->setDataType(&ddLDOUBLE);
			tr->source_text = lit_text;
			return tr;
		    }
		    if ( eat_imag_suffix() )
		    {
			TokenInt *ti = (TokenInt *)make_int((int64_t)(uint64_t)v);
			ti->source_text = lit_text;
			// gcc types an integer imaginary constant by the plain
			// decimal ladder: `4i` is _Complex int, not _Complex long.
			ti->setDataType(get_complex_compat_type(
			    v <= 0x7fffffffULL ? (DataDef *)&ddINT32
					       : (DataDef *)&ddINT64));
			return ti;
		    }
		    eat_int_suffix();
		    TokenInt *ti = finish_int_literal(v, v_ovf, is_octal);
		    ti->source_text = lit_text;
		    return ti;
		}
		// handle floating point
		lit_text += (char)source.get(); // eat .
		while ( source.good() )
		{
		    if ( is_digit_separator(source.peek()) )
		    {
			source.get();
			continue;
		    }
		    if ( !isdigit(source.peek()) )
			break;
		    lit_text += (char)source.get();
		}
		// Scientific notation exponent: e/E followed by optional +/- and digits
		if ( source.good() && (source.peek() == 'e' || source.peek() == 'E') )
		{
		    lit_text += (char)source.get(); // consume e/E
		    if ( source.good() && (source.peek() == '+' || source.peek() == '-') )
			lit_text += (char)source.get();
		    while ( source.good() && isdigit(source.peek()) )
			lit_text += (char)source.get();
		}
		// C float literal suffixes (f/F, l/L, the _FloatN family).
		// Preserve the spelling and stamp the TokenReal type below;
		// macro prescan and later lowering both need the suffix to
		// survive rather than silently widening it to double.
		eat_real_suffix(lit_text);
		long double num = strtold(lit_text.c_str(), NULL);
		if ( eat_imag_suffix() )
		{
		    TokenReal *tr = (TokenReal *)make_real(num);
		    char suffix = imag_type_suffix ? imag_type_suffix : real_type_suffix;
		    tr->setDataType(get_complex_compat_type(complex_real_type_for_suffix(suffix)));
		    std::string full = lit_text;
		    if (imag_type_suffix) { full += 'i'; full += imag_type_suffix; }
		    else full += 'i';
		    tr->source_text = full;
		    return tr;
		}
		{
		    TokenReal *tr = (TokenReal *)make_real(num);
		    // Record a single-precision type for an `f`/`F`-suffixed literal so
		    // its width survives into c2mir (it self-determines arithmetic type).
		    // Without this `1.0f` lowered as a double, which silently widened
		    // mixed float/float-_Complex arithmetic to double precision.
		    if ( real_suffix_dd )
			tr->setDataType(real_suffix_dd);
		    else if ( real_type_suffix == 'f' || real_type_suffix == 'F' )
			tr->setDataType(&ddFLOAT);
		    // An `L` literal is a long double, the same way `f` is a float:
		    // the suffix is the only thing that says so, and without this the
		    // value was parsed at full precision and then typed as a double.
		    else if ( real_type_suffix == 'l' || real_type_suffix == 'L' )
			tr->setDataType(&ddLDOUBLE);
		    tr->source_text = lit_text;
		    return tr;
		}
	    }
	    if ( ch == '_' || isalnum(ch) )
	    {
		word = "";
		word += ch;
		// Fold the spelling hash WHILE reading the identifier (tinycc model)
		// so intern() below doesn't re-walk the bytes for the hash.
		uint32_t whash = madc::dis::intern_table::hash_step(madc::dis::intern_table::hash_init(), (unsigned char)ch);

		// Fast path: scan the identifier-continuation SPAN straight off the
		// flat buffer (one append + a contiguous hash fold) instead of
		// per-char get()/peek() + std::string growth. Falls through to the
		// char loop below for the pushback (macro) and line-splice remainder.
		const char *idspan; size_t idlen;
		if ( source.fast_ident_span(idspan, idlen) )
		{
		    word.append(idspan, idlen);
		    for ( size_t k = 0; k < idlen; ++k )
			whash = madc::dis::intern_table::hash_step(whash, (unsigned char)idspan[k]);
		}
		while ( source.good() && (isalnum(source.peek()) || source.peek() == '_') )
		{
		    int wc = source.get();
		    word += (char)wc;
		    whash = madc::dis::intern_table::hash_step(whash, (unsigned char)wc);
		}
		// Prefixed literals ([lex.ccon]/[lex.string]): L / u / U / u8
		// ahead of a quote — the same prefix set the replacement-list
		// tokenizer accepts (libc++ unicode.h:
		// `U'�'`). Gate the C++11/20 prefixes on the dialect so a
		// C89 identifier `u` before a string stays two tokens.
		if ( source.good()
		  && (source.peek() == '"' || source.peek() == '\'')
		  && (word == "L"
		   || ((word == "u" || word == "U") && cpp_keyword_active(STD_CPP11))
		   || (word == "u8" && cpp_keyword_active(STD_CPP20))) )
		    return read_wide_literal(word);
		// Intern the spelling ONCE (with the pre-folded hash); reuse the id
		// for every per-word map probe below (macro/define/keyword/cpp-operator)
		// — a flat sid-indexed array access, no string compare, no tree.
		uint32_t sid = strpool.intern(word.data(), (uint32_t)word.size(), whash);
		// function-like macro expansion: NAME(args) or NAME (args)
		// Suppressed when the preceding tokens form a declaration /
		// definition head (`void bug(const char *, ...)` must not
		// be eaten by a prior `#define bug(...) ((void)0)`).
		if ( macro_map.count(sid) && !source.macro_disabled(word)
		     && consume_macro_call_open(source) )
		{
		    MacroDef &macro = macro_map[sid];
		    // read actual arguments (handling nested parens and strings)
		    std::vector<std::string> args;
		    std::string arg;
		    PpGroupScan group;
		    group.step('(', source.good() ? source.peek() : '\0');
		    bool at_line_start = false; // track whether next non-ws char is start of line
		    int macro_ifdef_skip = 0; // >0 means we're skipping a false #ifdef/#else branch
		    int macro_ifdef_depth = 0; // tracks #ifdef nesting inside macro args
		    while ( source.good() && !group.closed() )
		    {
			char mc = source.get();
			// Handle #ifdef/#ifndef/#else/#endif inside macro args
			// (GCC extension: conditional directives in macro args).
			if ( mc == '#' && at_line_start )
			{
			    // Read directive name
			    std::string dir;
			    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t') )
				source.get();
			    while ( source.good() && isalpha(source.peek()) )
				dir += source.get();
			    if ( dir == "ifdef" || dir == "ifndef" )
			    {
				while ( source.good() && (source.peek() == ' ' || source.peek() == '\t') )
				    source.get();
				std::string name;
				while ( source.good() && (isalnum(source.peek()) || source.peek() == '_') )
				    name += source.get();
				bool defined = macro_name_defined(name);
				bool active = (dir == "ifdef") ? defined : !defined;
				++macro_ifdef_depth;
				if ( !active )
				    ++macro_ifdef_skip;
				// consume rest of line
				while ( source.good() && source.peek() != '\n' && source.peek() != '\r' )
				    source.get();
				continue;
			    }
			    else if ( dir == "if" )
			    {
				// Evaluate #if condition
				bool active = evaluateIfCondition();
				++macro_ifdef_depth;
				if ( !active )
				    ++macro_ifdef_skip;
				continue;
			    }
			    else if ( dir == "else" )
			    {
				if ( macro_ifdef_skip > 0 )
				    --macro_ifdef_skip; // was skipping → now active
				else
				    ++macro_ifdef_skip; // was active → now skip
				while ( source.good() && source.peek() != '\n' && source.peek() != '\r' )
				    source.get();
				continue;
			    }
			    else if ( dir == "elif" )
			    {
				if ( macro_ifdef_skip > 0 )
				{
				    --macro_ifdef_skip;
				    bool active = evaluateIfCondition();
				    if ( !active )
					++macro_ifdef_skip;
				}
				else
				    ++macro_ifdef_skip; // was active → skip
				continue;
			    }
			    else if ( dir == "endif" )
			    {
				--macro_ifdef_depth;
				if ( macro_ifdef_skip > 0 )
				    --macro_ifdef_skip;
				while ( source.good() && source.peek() != '\n' && source.peek() != '\r' )
				    source.get();
				continue;
			    }
			    else
			    {
				// Not a conditional directive — put # + dir back as arg text
				arg += '#';
				arg += dir;
			    }
			    at_line_start = false;
			    continue;
			}
			at_line_start = (mc == '\n' || mc == '\r');
			// Skip content in false #ifdef/#else branch
			if ( macro_ifdef_skip > 0 )
			{
			    // Still need to track nested #ifdef/#endif in skipped text
			    continue;
			}
			bool structural = group.step(
			    mc, source.good() ? source.peek() : '\0');
			if ( structural && group.closed() )
			{
			    // The invocation's closing parenthesis is not argument text.
			}
			else if ( structural && mc == ',' && group.at_argument_level() )
			{
			    // trim whitespace from arg
			    while ( !arg.empty() && (arg.front() == ' ' || arg.front() == '\t') ) arg.erase(arg.begin());
			    while ( !arg.empty() && (arg.back() == ' ' || arg.back() == '\t') ) arg.pop_back();
			    args.push_back(arg);
			    arg.clear();
			}
			else arg += mc;
		    }
		    // If the active #ifdef branch contained the closing `)`,
		    // the remaining #else/#endif directives are still in the
		    // source. Buffer active content (before #else) and push
		    // it back; skip #else...#endif blocks entirely.
		    if ( macro_ifdef_depth > 0 )
		    {
			std::string preserved;
			while ( macro_ifdef_depth > 0 && source.good() )
			{
			    // Buffer content until a line starting with `#`
			    std::string line;
			    while ( source.good() && source.peek() != '\n' && source.peek() != '\r' )
				line += source.get();
			    // consume newline
			    std::string nl;
			    while ( source.good() && (source.peek() == '\n' || source.peek() == '\r') )
				nl += source.get();
			    // check if line is a directive
			    size_t p = 0;
			    while ( p < line.size() && (line[p] == ' ' || line[p] == '\t') ) ++p;
			    if ( p < line.size() && line[p] == '#' )
			    {
				++p;
				while ( p < line.size() && (line[p] == ' ' || line[p] == '\t') ) ++p;
				std::string dir2;
				while ( p < line.size() && isalpha(line[p]) ) dir2 += line[p++];
				if ( dir2 == "endif" )
				{
				    --macro_ifdef_depth;
				    // don't preserve the #endif line
				}
				else if ( dir2 == "else" || dir2 == "elif" )
				{
				    // Skip from #else/#elif to the matching #endif
				    int skip_depth = 1;
				    while ( skip_depth > 0 && source.good() )
				    {
					std::string sline;
					while ( source.good() && source.peek() != '\n' && source.peek() != '\r' )
					    sline += source.get();
					while ( source.good() && (source.peek() == '\n' || source.peek() == '\r') )
					    source.get();
					size_t sp = 0;
					while ( sp < sline.size() && (sline[sp] == ' ' || sline[sp] == '\t') ) ++sp;
					if ( sp < sline.size() && sline[sp] == '#' )
					{
					    ++sp;
					    while ( sp < sline.size() && (sline[sp] == ' ' || sline[sp] == '\t') ) ++sp;
					    std::string d3;
					    while ( sp < sline.size() && isalpha(sline[sp]) ) d3 += sline[sp++];
					    if ( d3 == "endif" ) --skip_depth;
					    else if ( d3 == "ifdef" || d3 == "ifndef" || d3 == "if" ) ++skip_depth;
					}
				    }
				    --macro_ifdef_depth;
				}
				else if ( dir2 == "ifdef" || dir2 == "ifndef" || dir2 == "if" )
				    ++macro_ifdef_depth;
			    }
			    else
			    {
				// Not a directive — preserve for the tokenizer
				preserved += line + nl;
			    }
			}
			if ( !preserved.empty() )
			    source.pushback(preserved);
		    }
		    // last argument
		    while ( !arg.empty() && (arg.front() == ' ' || arg.front() == '\t') ) arg.erase(arg.begin());
		    while ( !arg.empty() && (arg.back() == ' ' || arg.back() == '\t') ) arg.pop_back();
		    if ( !arg.empty() || !args.empty() )
			args.push_back(arg);
		    if ( looks_like_decl_head(tokens)
		      && decl_head_macro_args_look_like_prototype(args) )
		    {
			std::string tail("(");
			for ( size_t ai = 0; ai < args.size(); ++ai )
			{
			    if ( ai ) tail += ", ";
			    tail += args[ai];
			}
			tail += ")";
			source.pushback(tail);
			return make_ident(word);
		    }
		    // substitute params in body — single pass over the
		    // original body so an argument that happens to match a
		    // later parameter name doesn't get re-substituted. A
		    // per-param sequential sweep cascades: for
		    // `CREATE(type, T, 1)` where the user's variable is
		    // named `type` (macro's first arg), substituting
		    // `result→type` first and `type→T` next would rewrite
		    // the user's variable and corrupt the expansion.
		    // C standard: pre-expand an argument only where its parameter
		    // has an ordinary use. # and ## consume the raw spelling; an
		    // argument used only by those operators is never expanded.
		    // Keep both forms because one parameter may have both kinds of
		    // occurrence in the same body.
		    std::vector<std::string> raw_args = args;
		    bool has_named_varargs = macro.variadic && !macro.variadic_param.empty();
		    size_t fixed_param_count = macro_fixed_param_count(macro);
		    for ( size_t i = 0; i < args.size(); ++i )
		    {
			std::string &a = args[i];
			std::string param;
			if ( i < fixed_param_count )
			    param = macro.params[i];
			else if ( macro.variadic )
			    param = has_named_varargs ? macro.variadic_param : "__VA_ARGS__";
			if ( param.empty()
			  || !macro_param_has_expanded_use(macro, param) )
			    continue;
			// Quick check: does the argument contain any known macro name?
			// A naive alpha check triggers on hex literals (0x1F) and
			// integer suffixes (LU/ULL), whose round-trip through the
			// tokenizer loses the original representation.  Scan for
			// actual identifier words and see if any match a define.
			bool has_macro = false;
			std::vector<MacroReplacementToken> arg_tokens =
			    tokenize_macro_spelling(a);
			for ( size_t j = 0; j < arg_tokens.size() && !has_macro; ++j )
			    if ( arg_tokens[j].kind == MacroReplacementToken::rtIdentifier )
			    {
				std::string id = macro_token_text(a, arg_tokens[j]);
				if ( define_map.count(id) || macro_map.count(id) )
				    has_macro = true;
			    }
			if ( !has_macro ) continue;
			// Push arg text through the tokenizer to expand macros
			Source saved = std::move(source);
			source = Source();
			source.str(a);
			source.inherit_macro_disables(saved, word);
			std::string expanded_arg;
			TokenBase *at;
			while ( (at = getToken()) )
			{
			    switch ( at->type() )
			    {
				case TokenType::ttSpace: expanded_arg += ' '; break;
				case TokenType::ttTab:   expanded_arg += '\t'; break;
				case TokenType::ttEOL:   expanded_arg += '\n'; break;
				default:
				    expanded_arg += madc_token_spelling(at);
				    break;
			    }
			}
			source = std::move(saved);
			a = expanded_arg;
		    }
		    std::string expanded =
			expand_function_macro_body(macro, raw_args, args);
		    DBG(std::cout << "macro expand " << word << " -> " << expanded << std::endl);
		    source.pushback_macro(expanded, word);
		    return getToken();
		}
			// #define substitution: inject the define value into the source stream
			if ( define_map.count(sid) && !source.macro_disabled(word) )
			{
			    std::string &val = define_map[sid];
			    if ( !val.empty() )
			    {
			// Builtin libc aliases such as __builtin_strcmp -> strcmp
			// should resolve to the target identifier directly instead
			// of re-entering the macro rescanner. Otherwise user macros
			// like `#define strcmp __builtin_strcmp` recurse forever.
			if ( word.compare(0, 10, "__builtin_") == 0
			  && is_identifier_spelling(val) )
			    return make_ident(val);
			source.pushback_macro(val, word);
			return getToken(); // re-tokenize the substituted text
		    }
		    // empty define — skip and get next token
		    return getToken();
		}
		// Built-in predefined macros: __FILE__ and __LINE__.
		// Match C semantics — expand to a string literal of the current
		// filename and an integer constant of the current source line.
		// Users can still override via #define (handled above).
		if ( word == "__FILE__" )
		{
		    std::string quoted = "\"";
		    const char *fn = source.fname();
		    quoted += (fn ? fn : "<unknown>");
		    quoted += "\"";
		    source.pushback_macro(quoted, "");
		    return getToken();
		}
		if ( word == "__LINE__" )
		{
		    source.pushback_macro(std::to_string(source.line()), "");
		    return getToken();
		}
		// _Pragma("...") — the token form of #pragma (C99, C++11), routed
		// to the same handler the directive uses. It sits here, after the
		// define_map check above, so a user `#define _Pragma …` still wins,
		// exactly as it does for __FILE__ / __LINE__. Deliberately NOT gated
		// on --std=: both canon compilers accept _Pragma in every mode,
		// -std=c89 -pedantic included (its name is reserved to the
		// implementation, so it can never collide with user code).
		if ( word == "_Pragma" )
		{
		    handle_pragma_operator();
		    return getToken();
		}
		// __FUNCTION__ / __func__ / __PRETTY_FUNCTION__: keep these
		// as magic identifiers. madc tokenizes the whole file before
		// parsing, so parseExpression resolves them after cur_func_name
		// is known.
		if ( word == "__FUNCTION__" || word == "__func__"
		  || word == "__PRETTY_FUNCTION__" )
		    return make_ident(word);
		// noexcept in NON-C++ modes only: strip the optional (...) by
		// BALANCED parens — NOT via a function-like macro, whose
		// comma-splitting breaks on a template-id condition such as
		// noexcept(is_nothrow_constructible<T, Args...>::value) (the
		// preprocessor does not treat <...> as grouping). In C++ modes /
		// the madc dialect, noexcept is a reserved TokenCppKeyword
		// (cpp_reserved) serving both the [except.spec] specifier and the
		// [expr.unary.noexcept] operator, so it falls through to the
		// keyword_map lookup below.
		if ( word == "noexcept" && !cpp_keyword_active(STD_CPP11) )
		{
		    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t' || source.peek() == '\n' || source.peek() == '\r') )
			source.get();
		    if ( source.peek() == '(' )
		    {
			consume_pp_group(source);
			return getToken();
		    }
		    // An UNCONDITIONAL `noexcept` re-lexes as its token
		    // equivalent `throw()` ([except.spec]p1: both declare the
		    // function non-throwing) so the parser's exception-spec arm
		    // records FuncDef::NxTrue — full erasure left every user
		    // ctor's nothrow-ness unknowable to
		    // __is_nothrow_constructible.
		    source.pushback("throw()");
		    return getToken();
		}
		// Most GCC attributes are no-ops for madc. Preserve the few
		// layout/type/lookup-shaping ones the parser understands and skip
		// the rest.
		if ( word == "__attribute__" || word == "__attribute" )
		{
		    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t' || source.peek() == '\n' || source.peek() == '\r') )
			source.get();
		    std::string attr_text;
		    if ( source.peek() == '(' )
			consume_pp_group(source, &attr_text);
		    if ( gnu_attribute_text_has_supported_name(attr_text) )
		    {
			source.pushback(attr_text);
			return make_ident(word);
		    }
		    return getToken();
		}
		// GCC/clang predefine __int128_t / __uint128_t as typedefs for
		// (unsigned) __int128 — always available, no header. They are
		// ATOMIC: unlike the __int128 keyword they do NOT combine with
		// signed/unsigned/long (`unsigned __int128_t` is invalid), so
		// resolve them directly to the canonical 128-bit datatype rather
		// than feeding the compound accumulator. The datatype name stays
		// "__int128" (from ddINT128), so downstream name-keyed lookups and
		// c2mir emission are identical to the __int128 keyword path.
		if ( word == "__int128_t" )
		    return make_datatype("__int128", ddINT128);
		if ( word == "__uint128_t" )
		    return make_datatype("unsigned __int128", ddUINT128);
		// The compiler-owned SysV va_list (struct __madc_va_list_tag[1]
		// singleton) — ATOMIC like __int128_t; the parser's spelling
		// table (resolve_builtin_type_spelling) returns the same object.
		if ( word == "__builtin_va_list" )
		    return make_datatype("__builtin_va_list",
					 *use_builtin_va_list());
		// Compound type specifiers: any mix of unsigned/signed/long/
		// short/int/char/double in any order (C99 6.7.2).
		// Uses a bitmap accumulator (chibicc-style) so order doesn't
		// matter: `unsigned long long int` = `long unsigned int long`.
		if ( word == "unsigned"   || word == "signed"
		  || word == "long"       || word == "short"
		  || word == "int"        || word == "char"
		  || word == "double"     || word == "float"
		  || word == "__int128"
		  || word == "_Complex"   || word == "__complex__"
		  || word == "__complex"
		  || word == "_Float16"   || word == "_Float32"
		  || word == "_Float64"   || word == "_Float128"
		  || word == "_Float32x"  || word == "_Float64x" )
		{
		    enum {
			TS_VOID     = 1 << 0,
			TS_CHAR     = 1 << 2,
			TS_SHORT    = 1 << 4,
			TS_INT      = 1 << 6,
			TS_LONG     = 1 << 8,  // two LONGs = LONG+LONG
			TS_FLOAT    = 1 << 10,
			TS_DOUBLE   = 1 << 12,
			TS_SIGNED   = 1 << 14,
			TS_UNSIGNED = 1 << 16,
			TS_COMPLEX  = 1 << 18,
			TS_INT128   = 1 << 20,
			TS_FLOATN_F = 1 << 22,	// _Float16/_Float32 (~float)
			TS_FLOATN_D = 1 << 24,	// _Float64/.../_Float64x (~double)
		    };
		    int counter = compound_type_specifier_flag(word);
		    // Accumulate subsequent type-specifier keywords.
		    // ws_count reports the whitespace consumed BEFORE the
		    // word: a rejected lookahead must give it back (as one
		    // normalized space), or the column count AND the next
		    // token's leading trivia lose it (`char *s` -> `char*s`).
		    auto read_word = [&](int &ws_count) -> std::string {
			ws_count = 0;
			while ( source.good()
			     && (source.peek() == ' ' || source.peek() == '\t'
			      || source.peek() == '\n' || source.peek() == '\r') )
			{
			    source.get();
			    ++ws_count;
			}
			std::string w;
			while ( source.good() && (isalnum(source.peek()) || source.peek() == '_') )
			    w += source.get();
			return w;
		    };
		    // Read ahead, accumulating type specifier keywords
		    std::vector<std::string> consumed;
		    while ( true )
		    {
			int ws_count = 0;
			std::string w = read_word(ws_count);
			int flag = compound_type_specifier_flag(w);
			if ( flag )
			{
			    counter += flag;
			    consumed.push_back(w);
			}
			else if ( !w.empty()
			       && define_map.find(w) != define_map.end() )
			{
			    int expanded_flags = 0;
			    if ( expansion_is_compound_type_specifiers(define_map[w], expanded_flags) )
			    {
				counter += expanded_flags;
				consumed.push_back(w);
			    }
			    else
			    {
				source.pushback_reread(std::string(" ") + w);
				break;
			    }
			}
			else
			{
			    // Not a type specifier — push it back, and give
			    // back consumed whitespace even when NO word
			    // followed it.
			    if ( !w.empty() )
				source.pushback_reread(std::string(" ") + w);
			    else if ( ws_count > 0 )
				source.pushback_reread(std::string(" "));
			    break;
			}
		    }
		    // Resolve accumulated type specifiers to DataDef
		    int normalized_counter = counter & ~TS_COMPLEX;
		    switch ( normalized_counter )
		    {
			case 0:
			    if ( counter & TS_COMPLEX )
			    {
				DataDef *complex_dd = get_complex_compat_type(&ddDOUBLE);
				return make_datatype(complex_dd->name.c_str(), *complex_dd);
			    }
			    break;
			case TS_CHAR:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddCHAR); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("char", ddCHAR);
			case TS_SIGNED + TS_CHAR:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT8); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("signed char", ddINT8);
			case TS_UNSIGNED + TS_CHAR:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT8); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned char", ddUINT8);
			case TS_SHORT:
			case TS_SHORT + TS_INT:
			case TS_SIGNED + TS_SHORT:
			case TS_SIGNED + TS_SHORT + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT16); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("short", ddINT16);
			case TS_UNSIGNED + TS_SHORT:
			case TS_UNSIGNED + TS_SHORT + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT16); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned short", ddUINT16);
			case TS_INT:
			case TS_SIGNED:
			case TS_SIGNED + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT32); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("int", ddINT32);
			case TS_UNSIGNED:
			case TS_UNSIGNED + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT32); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned int", ddUINT32);
			case TS_LONG:
			case TS_LONG + TS_INT:
			case TS_SIGNED + TS_LONG:
			case TS_SIGNED + TS_LONG + TS_INT:
			    // Plain `long` is target-shaped (4-byte on LLP64) —
			    // dd_platform_long(); the LONG+LONG rows below stay i64.
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(dd_platform_long()); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("long", *dd_platform_long());
			case TS_UNSIGNED + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(dd_platform_ulong()); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned long", *dd_platform_ulong());
			case TS_LONG + TS_LONG:
			case TS_LONG + TS_LONG + TS_INT:
			case TS_SIGNED + TS_LONG + TS_LONG:
			case TS_SIGNED + TS_LONG + TS_LONG + TS_INT:
			    // `long long` is target-shaped like plain `long`
			    // above: distinct from long (mangles x) on the
			    // LP64 darwin model, ddINT64 everywhere else.
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(dd_platform_longlong()); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("long long", *dd_platform_longlong());
			case TS_UNSIGNED + TS_LONG + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(dd_platform_ulonglong()); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned long long", *dd_platform_ulonglong());
			case TS_INT128:
			case TS_SIGNED + TS_INT128:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT64); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("__int128", ddINT128);
			case TS_UNSIGNED + TS_INT128:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT64); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned __int128", ddUINT128);
			case TS_FLOAT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddFLOAT); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("float", ddFLOAT);
			case TS_DOUBLE:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddDOUBLE); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("double", ddDOUBLE);
			case TS_LONG + TS_DOUBLE:
			    // ddLDOUBLE, not ddDOUBLE: `long double` is its own type
			    // (x87 80-bit on x86-64). Mapping it to the double DataDef
			    // made sizeof 8, broke printf("%Lg") at the varargs
			    // boundary, and left the mangler emitting Itanium `e` for a
			    // value passed as a double.
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddLDOUBLE); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("long double", ddLDOUBLE);
			// C23 _FloatN + _Complex, either order (gcc's
			// avx512fp16intrin.h: `_Float16 _Complex __A`). The complex
			// carrier rides the same nearest-supported approximation the
			// bare spellings use (add_datatypes: _Float16/_Float32 ~
			// float, the rest ~ double; ABI fidelity = the Tier-3 half-
			// float floor gap). BARE _FloatN breaks to the default's
			// fall-through, where the datatype_map's own spelling-named
			// token serves it exactly as before.
			case TS_FLOATN_F:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddFLOAT); return make_datatype(dd->name.c_str(), *dd); }
			    break;
			case TS_FLOATN_D:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddDOUBLE); return make_datatype(dd->name.c_str(), *dd); }
			    break;
			default:
			    // Unrecognized combination — push back consumed words
			    // in reverse and fall through to identifier/keyword lookup.
			    for ( auto it = consumed.rbegin(); it != consumed.rend(); ++it )
				source.pushback_reread(std::string(" ") + *it);
			    break;
		    }
		}
		if ( (kmi=keyword_map.find(sid)) != keyword_map.end() )
		    return (*kmi)->clone();
		// C++ alternative-token operators (and/or/not/...): empty in C
		// mode, so this find is a no-op there.
		if ( !cpp_operator_map.empty() )
		{
		    madc::dis::intern_keyed_map<TokenBase *>::iterator oi =
			cpp_operator_map.find(sid);
		    if ( oi != cpp_operator_map.end() )
			return (*oi)->clone();
		}
		if ( (bmi=datatype_map.find(word)) != datatype_map.end() )
		    return (*bmi)->clone();
		if ( auto_include_standard_identifier(word) )
		    return getToken();
		TokenIdent *ti = (TokenIdent *)make_ident(word);
		ti->rec.spelling_id = sid;   // already interned above; skip re-intern in getToken()
		return ti;
	    }
	    return make_char(ch);
	// end switch
    }

    return NULL;
}

// skip tokens in a false #ifdef/#ifndef/#if/#elif/#else block
// handles nested #if/#ifdef/#ifndef blocks; returns when matching #else/#elif/#endif found
TokenBase *Program::skipConditionalBlock()
{
    int depth = 0;
    while ( source.good() && !source.eof() )
    {
	// skip leading horizontal whitespace; directives may be indented
	while ( source.good() && !source.eof() && (source.peek() == ' ' || source.peek() == '\t') )
	    source.get();
	if ( !source.good() || source.eof() )
	    break;
	char ch = source.get();
	if ( ch == '\n' || ch == '\r' )
	    continue;
	if ( ch != '#' )
	{
	    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		source.get();
	    continue;
	}
	// skip whitespace after #
	while ( source.peek() == ' ' || source.peek() == '\t' )
	    source.get();
	// read directive word
	std::string dir;
	while ( source.good() && !source.eof() && isalpha(source.peek()) )
	    dir += source.get();
	if ( dir == "ifdef" || dir == "ifndef" || dir == "if" )
	{
	    depth++;
	    // consume the rest of the directive line (comment-aware)
	    skip_directive_line_tail(source);
	}
	else if ( dir == "endif" )
	{
	    // consume the rest of the directive line (comment-aware)
	    skip_directive_line_tail(source);
	    DBG(std::cout << "skipConditionalBlock: #endif depth=" << depth << " stack=" << ifdef_stack.size() << std::endl);
	    if ( depth == 0 )
	    {
		// this #endif closes our block
		ifdef_stack.pop();
		ifdef_done_stack.pop();
		DBG(std::cout << "skipConditionalBlock: popped, stack now=" << ifdef_stack.size() << std::endl);
		return getToken();
	    }
	    depth--;
	}
	else if ( depth == 0 && dir == "else" )
	{
	    // consume the rest of the directive line (comment-aware)
	    skip_directive_line_tail(source);
	    bool already_done = ifdef_done_stack.top();
	    ifdef_stack.pop();
	    bool active = !already_done;
	    ifdef_stack.push(active);
	    if ( active )
		ifdef_done_stack.top() = true;
	    if ( active )
		return getToken();
	    // still false, keep skipping
	}
	else if ( depth == 0 && dir == "elif" )
	{
	    // do NOT consume rest of line — evaluateIfCondition() needs to read the condition
	    bool already_done = ifdef_done_stack.top();
	    ifdef_stack.pop();
	    if ( already_done )
	    {
		ifdef_stack.push(false);
		// consume the rest of the directive line (comment-aware) since
		// we won't evaluate this #elif
		skip_directive_line_tail(source);
		// keep skipping
	    }
	    else
	    {
		bool active = evaluateIfCondition();
		ifdef_stack.push(active);
		if ( active )
		    ifdef_done_stack.top() = true;
		DBG(std::cout << "#elif (in skip) -> " << (active ? "true" : "false") << std::endl);
		if ( active )
		    return getToken();
		// still false, keep skipping
	    }
	}
	else
	{
	    // consume rest of line for unknown directives inside skipped block
	    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		source.get();
	}
    }
    Throw << "Unterminated conditional compilation block" << flush;
    return NULL;
}

// evaluate #if condition: supports defined(NAME), !, &&, ||, ?:, the
// comparison/arithmetic/bitwise/shift tiers, identifiers, and integer
// constants — the full C #if expression grammar real glibc/libstdc++
// headers use.
// Expand all #define macros in a #if/#elif expression string.
// Simple defines are replaced with their values; undefined identifiers
// (other than 'defined') become 0 per the C standard. Runs up to
// max_depth iterations to handle chained expansions.
std::string Program::expandIfMacros(const std::string &raw)
{
    std::string expr = raw;
    for ( int depth = 0; depth < 32; ++depth )
    {
	std::string out;
	bool changed = false;
	bool preserve_defined_operand = false;
	std::vector<MacroReplacementToken> expression_tokens =
	    tokenize_macro_spelling(expr);
	for ( size_t ti = 0; ti < expression_tokens.size(); )
	{
	    const MacroReplacementToken &token = expression_tokens[ti];
	    if ( token.kind != MacroReplacementToken::rtIdentifier )
	    {
		if ( token.kind == MacroReplacementToken::rtComment )
		    out += ' ';
		else
		    out += macro_token_text(expr, token);
		++ti;
		continue;
	    }
	    std::string word = macro_token_text(expr, token);
	    // `defined` and the clang `__has_*` family are #if OPERATORS, not
	    // macros, and their operands are NOT macro-expanded. That is not a
	    // fine point for madc: it aliases 138 builtins in define_map
	    // (__builtin_memcpy -> memcpy), so expanding the operand would turn
	    // __has_builtin(__builtin_memcpy) into __has_builtin(memcpy) and
	    // answer NO for a builtin madc implements.
	    bool is_has_op = word.size() > 6 && word.compare(0, 6, "__has_") == 0;
	    // B4a: every identifier a #if/#elif condition consults (including
	    // `defined` operands, which the evaluator resolves later) is a
	    // branch-relevant macro name for the pack container. The operators
	    // themselves are not macros and are not recorded.
	    if ( word != "defined" && !is_has_op )
		pack_record_branch_macro(word);
	    if ( word == "defined" )
	    {
		out += word;
		preserve_defined_operand = true;
		++ti;
		continue;
	    }
	    if ( is_has_op )
	    {
		out += word;
		// Copy the whole parenthesized operand through verbatim — one
		// group, so `<a/b.h>`, `"a.h"` and `clang::foo` all survive
		// intact for the operator to interpret.
		size_t group_token = ti + 1;
		while ( group_token < expression_tokens.size()
		     && macro_token_space(expression_tokens[group_token]) )
		    ++group_token;
		if ( group_token < expression_tokens.size()
		  && expression_tokens[group_token].kind == MacroReplacementToken::rtPunct
		  && macro_token_text(expr, expression_tokens[group_token]) == "(" )
		{
		    size_t end = pp_group_end(expr,
			expression_tokens[group_token].begin);
		    if ( end != std::string::npos )
		    {
			out += expr.substr(token.end, end - token.end);
			while ( ti < expression_tokens.size()
			     && expression_tokens[ti].begin < end )
			    ++ti;
			continue;
		    }
		}
		++ti;
		continue;
	    }
	    if ( preserve_defined_operand )
	    {
		out += word;
		preserve_defined_operand = false;
		++ti;
		continue;
	    }
	    // Look up in define_map
	    auto it = define_map.find(word);
	    if ( it != define_map.end() )
	    {
		out += it->empty() ? "1" : *it;
		changed = true;
		++ti;
	    }
	    else if ( macro_map.count(word) > 0 )
	    {
		// Function-like macro: expand a real invocation by collecting
		// its balanced argument list from the expression text and
		// substituting parameters into the body (the standard requires
		// full macro expansion in #if). Replacing the call with a bare
		// "1" left the argument list behind as garbage tokens —
		// `__GNUC_PREREQ (7, 0) && !defined X` became `1 (7, 0) && …`,
		// derailing the evaluator so the defined-operator tail
		// produced the wrong branch (glibc's floatn-common.h
		// __HAVE_FLOATN_NOT_TYPEDEF condition).
		size_t group_token = ti + 1;
		while ( group_token < expression_tokens.size()
		     && macro_token_space(expression_tokens[group_token]) )
		    ++group_token;
		if ( group_token < expression_tokens.size()
		  && expression_tokens[group_token].kind == MacroReplacementToken::rtPunct
		  && macro_token_text(expr, expression_tokens[group_token]) == "(" )
		{
		    const MacroDef &m = macro_map[word];
		    std::vector<std::string> margs;
		    std::string marg;
		    PpGroupScan mgroup;
		    size_t j = expression_tokens[group_token].begin;
		    mgroup.step('(', j + 1 < expr.size() ? expr[j + 1] : '\0');
		    ++j; // consume '('
		    for ( ; j < expr.size(); ++j )
		    {
			char mc = expr[j];
			char next = j + 1 < expr.size() ? expr[j + 1] : '\0';
			bool structural = mgroup.step(mc, next);
			if ( structural && mgroup.closed() )
			{
			    ++j;
			    break;
			}
			else if ( structural && mc == ','
			       && mgroup.at_argument_level() )
			{ margs.push_back(marg); marg.clear(); }
			else marg += mc;
		    }
		    if ( !marg.empty() || !margs.empty() )
			margs.push_back(marg);
		    auto trim = [](std::string &s) {
			while ( !s.empty() && (s.front()==' '||s.front()=='\t') ) s.erase(s.begin());
			while ( !s.empty() && (s.back()==' '||s.back()=='\t') ) s.pop_back();
		    };
		    for ( auto &a : margs ) trim(a);
		    std::vector<std::string> expanded_args = margs;
		    size_t fixed = macro_fixed_param_count(m);
		    for ( size_t ai = 0; ai < expanded_args.size(); ++ai )
		    {
			std::string param;
			if ( ai < fixed )
			    param = m.params[ai];
			else if ( m.variadic )
			    param = m.variadic_param.empty()
				? "__VA_ARGS__" : m.variadic_param;
			if ( !param.empty() && macro_param_has_expanded_use(m, param) )
			    expanded_args[ai] = expandIfMacros(margs[ai]);
		    }
		    std::string expanded =
			expand_function_macro_body(m, margs, expanded_args);
		    out += "(" + expanded + ")";
		    while ( ti < expression_tokens.size()
			 && expression_tokens[ti].begin < j )
			++ti;
		    changed = true;
		}
		else
		{
		    // A function-like macro name without an invocation is not
		    // expanded. It may become callable after parameter substitution.
		    out += word;
		    ++ti;
		}
	    }
	    else if ( word == "__LINE__" )
	    {
		// Predefined macros live in getToken's builtin arm, not in
		// define_map — the string expander needs its own arm or a
		// `#if N != __LINE__` / `#line line` operand stays an
		// identifier and evaluates as 0 (c-testsuite 00152). The
		// define_map probe above ran first, so a user #define of
		// the name still wins, matching getToken's order.
		out += std::to_string(source.line());
		changed = true;
		++ti;
	    }
	    else if ( word == "__FILE__" )
	    {
		out += '"';
		out += source.fname() ? source.fname() : "<unknown>";
		out += '"';
		changed = true;
		++ti;
	    }
	    else
	    {
		out += word; // leave as-is (will become 0 in the evaluator)
		++ti;
	    }
	}
	expr = out;
	if ( !changed ) break;
    }
    return expr;
}

// --- the clang `__has_*` preprocessor operators ------------------------------
//
// A modern standard library does not merely *prefer* compiler intrinsics — it
// asks for them and then commits: libc++ 18 queries `__has_builtin` 122 times
// and, where it has no fallback, `#error`s outright. madc answered every one
// of those by falling through parse_primary's "unknown identifier = 0", which
// is the right ANSWER for a builtin it lacks but arrived by accident, and left
// `__has_include` — a question madc can answer exactly — answering no.
//
// THE RULE FOR EVERY QUERY HERE: answer from madc's own state, and answer
// truthfully. A yes madc cannot back trades a library's clean "not
// implemented" #error for a mystifying failure deep inside its headers. When
// in doubt the answer is 0: that costs a fast path, never correctness.
bool Program::has_builtin(const std::string &name)
{
    // Compiler type-trait intrinsics carry no __builtin_ prefix, and madc
    // implements a real subset of them (__is_class, __has_trivial_destructor,
    // …). Answer from THAT registry — it is the same "answer from madc's own
    // state" contract, applied to a second kind of state. Saying no here is
    // what made libc++ #error on a trait madc had all along.
    if ( is_type_trait_builtin(name) )
	return true;
    // Builtin TEMPLATES madc implements natively — same "answer from madc's
    // own state" contract (instantiate_make_integer_seq is the state). libc++
    // takes its integer_sequence BUILTIN branch on this answer.
    if ( name.compare("__make_integer_seq") == 0 )
	return true;
    // __type_pack_element<I, Types...> — folded natively at
    // instantiate_template_use (the I-th type IS the result; no
    // instantiation). libc++'s tuple_element takes its builtin branch on
    // this answer instead of the decltype-indexer fallback, whose
    // resolution minted an opaque dependent shell (task #103).
    if ( name.compare("__type_pack_element") == 0 )
	return true;
    if ( name.compare(0, 10, "__builtin_") != 0 )
	return false;	// trait intrinsics madc does NOT implement
			// (__remove_reference_t and the rest of libc++'s 41)
			// answer no — see
			// docs/plans/2026-07-26-libcxx-flavor-plan.md
    // The alias table IS madc's builtin implementation for this family: each
    // entry rewrites the call to the libc function that implements it, so
    // membership is exactly "madc compiles a call to this". -fno-builtin-foo
    // deliberately does NOT change the answer, matching clang: the flag
    // suppresses the optimization, it does not unimplement the builtin.
    return define_map.count(name) > 0 || macro_map.count(name) > 0;
}

// The `__has_*` operators madc ANSWERS from its own state. This is the single
// list behind two questions that must never disagree: what evaluateHasQuery
// will answer, and what `#ifdef` can see. madc previously answered
// __has_builtin correctly while `#ifdef __has_builtin` said NO — and
// libstdc++ wraps its whole _GLIBCXX_HAS_BUILTIN family in exactly that ifdef
// (c++config.h:830), so every guard below it silently evaluated to 0 and the
// default lane quietly lost LAUNDER / IS_SAME / HAS_UNIQ_OBJ_REP and their
// siblings. __has_attribute now answers from gnu_attribute_kind; operators
// with no truthful registry (__has_feature, …) stay OFF this list and thus
// invisible.
bool Program::has_query_operator_implemented(const std::string &op)
{
    return op == "__has_builtin"
	|| op == "__has_attribute"
	|| op == "__has_include"
	|| op == "__has_include_next";
}

bool Program::macro_name_defined(const std::string &name)
{
    return define_map.count(name) > 0 || macro_map.count(name) > 0
	|| has_query_operator_implemented(name);
}

int64_t Program::evaluateHasQuery(const std::string &op, const std::string &expr,
				  size_t &pos)
{
    if ( !has_query_operator_implemented(op) )
	return 0;	// see has_query_operator_implemented — deliberate 0
    while ( pos < expr.size() && (expr[pos] == ' ' || expr[pos] == '\t') )
	++pos;
    if ( pos >= expr.size() || expr[pos] != '(' )
	return 0;	// bare identifier: no query to answer
    // Take the argument as raw text to the matching ')': the forms differ per
    // operator (`<a/b.h>`, `"a.h"`, `clang::foo`, a bare identifier), so the
    // shape belongs to the operator, not to this scanner.
    size_t open = pos;
    size_t end = pp_group_end(expr, open);
    if ( end == std::string::npos )
	return 0;
    std::string arg = expr.substr(open + 1, end - open - 2);
    pos = end;
    size_t b = arg.find_first_not_of(" \t");
    size_t e = arg.find_last_not_of(" \t");
    arg = (b == std::string::npos) ? std::string() : arg.substr(b, e - b + 1);
    if ( arg.empty() )
	return 0;

    if ( op == "__has_builtin" )
	return has_builtin(arg) ? 1 : 0;

    if ( op == "__has_attribute" )
    {
	// Preserving an attribute is not enough to advertise the complete
	// compiler contract for it.  Grow this truth set only with an
	// oracle-backed semantic gate; using_if_exists has both.
	return madc_gnu_attribute_kind(arg) == GnuAttributeKind::UsingIfExists ? 1 : 0;
    }

    if ( op == "__has_include" || op == "__has_include_next" )
    {
	// Answered EXACTLY, by the same resolution `#include` itself uses — so
	// "can I include this?" and "will including it work?" can never
	// disagree. That resolution consults the NAMED providers (embedded
	// text, baked PCH) before the filesystem, through the same gates; a
	// file-open probe alone said NO to names madc itself serves (libc++'s
	// __mbstate_t.h asks __has_include_next(<wchar.h>) and #errors on the
	// honest-but-wrong answer — the hosted-darwin prelude is on no disk).
	bool is_system = arg[0] == '<';
	if ( (is_system && arg.back() != '>')
	  || (!is_system && (arg[0] != '"' || arg.back() != '"')) )
	    return 0;
	std::string file = arg.substr(1, arg.size() - 2);
	if ( file.empty() )
	    return 0;
	if ( is_system && is_posix_compat_header_name(file) )
	    return 0;
	if ( is_system )
	{
	    if ( op == "__has_include_next" )
	    {
		if ( embedded_wins_include_next(file) )
		    return 1;
	    }
	    else
	    {
		std::string pch_path;
		if ( named_include_provider_exists(*this, file, pch_path) )
		    return 1;
	    }
	}
	std::string path = op == "__has_include_next"
			 ? resolve_include_next_path(file)
			 : resolve_include_path(file, is_system);
	if ( resolved_include_provider_exists(path) )
	    return 1;
	// LAST position in the executor's resolution order, mirrored here: a
	// POSIX name the native toolchain ships no header for at all is served
	// by the posix/ entry AFTER the walk fails. Same predicate, same
	// position, so the answer matches what #include will do.
	if ( is_system && op == "__has_include"
	  && posix_whole_provider_serves(file, path) )
	    return 1;
	return 0;
    }

    // __has_cpp_attribute / __has_declspec_attribute / __has_feature /
    // __has_extension / __has_keyword: there is no truthful registry to ask
    // yet.  0 keeps every library on its portable path.
    return 0;
}

bool Program::evaluateIfCondition()
{
    std::string raw_expr;
    // Translation-phase-3 comment replacement ON the captured condition: each
    // comment becomes one space BEFORE the string evaluator runs. A trailing
    // `/* in libmingwex.a */` otherwise reaches the expression parser as
    // division-and-garbage and the condition silently evaluates FALSE —
    // mingw's wchar.h/stdlib.h guard their ISO-C-ext declarations (wcstold,
    // strtold, llabs) with exactly that shape, and the dropped declarations
    // surface as "not a declaration in '::'" a thousand lines later. It also
    // keeps comment words out of pack_record_branch_macro (B4a). A block
    // comment may span physical lines (the <stddef.h> #endif precedent —
    // consume through `*/` wherever it closes); char/string literals shield
    // both comment introducers.
    bool in_str = false, in_chr = false;
    while ( source.good() && !source.eof() )
    {
	int ch = source.peek();
	if ( ch == '\n' || ch == '\r' )
	    break;
	if ( !in_str && !in_chr && ch == '/' )
	{
	    source.get();
	    int nx = source.peek();
	    if ( nx == '*' )
	    {
		source.get();
		int prev = 0;
		while ( source.good() && !source.eof() )
		{
		    int c2 = source.get();
		    if ( prev == '*' && c2 == '/' )
			break;
		    prev = c2;
		}
		raw_expr += ' ';
		continue;
	    }
	    if ( nx == '/' )
	    {
		while ( source.good() && !source.eof()
		     && source.peek() != '\n' && source.peek() != '\r' )
		    source.get();
		break;
	    }
	    raw_expr += '/';
	    continue;
	}
	source.get();
	if ( in_str )
	{
	    if ( ch == '\\' && source.good() && !source.eof()
	      && source.peek() != '\n' && source.peek() != '\r' )
	    {
		raw_expr += (char)ch;
		ch = source.get();
	    }
	    else if ( ch == '"' )
		in_str = false;
	}
	else if ( in_chr )
	{
	    if ( ch == '\\' && source.good() && !source.eof()
	      && source.peek() != '\n' && source.peek() != '\r' )
	    {
		raw_expr += (char)ch;
		ch = source.get();
	    }
	    else if ( ch == '\'' )
		in_chr = false;
	}
	else if ( ch == '"' )
	    in_str = true;
	else if ( ch == '\'' )
	    in_chr = true;
	raw_expr += (char)ch;
    }

    // Expand macros before evaluation so expressions like
    // OPENSSL_VERSION_MAJOR * 10000 + OPENSSL_VERSION_MINOR * 100
    // resolve to numeric values.
    std::string expr = expandIfMacros(raw_expr);
    DBG(std::cout << "#if expand: " << raw_expr << " → " << expr << std::endl);

    size_t pos = 0;

    auto skip_ws = [&]() {
	while ( pos < expr.size() && (expr[pos] == ' ' || expr[pos] == '\t') )
	    ++pos;
    };

    // Integer-valued recursive descent so comparisons work correctly.
    std::function<int64_t()> parse_cond;
    std::function<int64_t()> parse_or;
    std::function<int64_t()> parse_and;
    std::function<int64_t()> parse_comparison;
    std::function<int64_t()> parse_unary;
    std::function<int64_t()> parse_primary;

    auto read_char_escape = [&]() -> int64_t {
	if ( pos >= expr.size() )
	    return 0;
	char esc = expr[pos++];
	switch ( esc )
	{
	    case 'n':  return '\n';
	    case 't':  return '\t';
	    case 'r':  return '\r';
	    case '\\': return '\\';
	    case '"':  return '"';
	    case '\'': return '\'';
	    case 'a':  return '\a';
	    case 'b':  return '\b';
	    case 'f':  return '\f';
	    case 'v':  return '\v';
	    case '?':  return '\?';
	    case 'x': case 'X': {
		int64_t val = 0;
		while ( pos < expr.size() && isxdigit((unsigned char)expr[pos]) )
		{
		    char c = expr[pos++];
		    int d = (c >= '0' && c <= '9') ? c - '0'
			: (c >= 'a' && c <= 'f') ? c - 'a' + 10
			: (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
		    val = (val << 4) | d;
		}
		return val;
	    }
	    case '0': case '1': case '2': case '3':
	    case '4': case '5': case '6': case '7': {
		int64_t val = esc - '0';
		int dig = 1;
		while ( dig < 3 && pos < expr.size()
		     && expr[pos] >= '0' && expr[pos] <= '7' )
		{
		    val = (val << 3) | (expr[pos++] - '0');
		    ++dig;
		}
		return val;
	    }
	    default:
		return (unsigned char)esc;
	}
    };

    auto read_char_literal = [&](bool wide) -> int64_t {
	if ( wide )
	    ++pos;
	if ( pos >= expr.size() || expr[pos] != '\'' )
	    return 0;
	++pos;
	int64_t value = 0;
	while ( pos < expr.size() && expr[pos] != '\'' )
	{
	    int64_t ch;
	    if ( expr[pos] == '\\' )
	    {
		++pos;
		ch = read_char_escape();
	    }
	    else
		ch = (unsigned char)expr[pos++];
	    value = (value << 8) | (ch & 0xff);
	    if ( wide )
		value = ch;
	}
	if ( pos < expr.size() && expr[pos] == '\'' )
	    ++pos;
	return value;
    };

    parse_primary = [&]() -> int64_t {
	skip_ws();
	if ( pos >= expr.size() )
	    return 0;

	if ( expr[pos] == '(' )
	{
	    ++pos;
	    int64_t value = parse_cond();
	    skip_ws();
	    if ( pos < expr.size() && expr[pos] == ')' )
		++pos;
	    return value;
	}

	if ( pos + 1 < expr.size() && expr[pos] == 'L' && expr[pos+1] == '\'' )
	    return read_char_literal(true);

	if ( expr[pos] == '\'' )
	    return read_char_literal(false);

	// hex literal 0x...
	if ( pos + 1 < expr.size() && expr[pos] == '0'
	  && (expr[pos+1] == 'x' || expr[pos+1] == 'X') )
	{
	    pos += 2;
	    int64_t val = 0;
	    while ( pos < expr.size() && isxdigit((unsigned char)expr[pos]) )
	    {
		val <<= 4;
		char c = expr[pos++];
		if ( c >= '0' && c <= '9' ) val += c - '0';
		else if ( c >= 'a' && c <= 'f' ) val += c - 'a' + 10;
		else if ( c >= 'A' && c <= 'F' ) val += c - 'A' + 10;
	    }
	    // skip integer suffix
	    while ( pos < expr.size() && (expr[pos] == 'u' || expr[pos] == 'U'
		 || expr[pos] == 'l' || expr[pos] == 'L') )
		++pos;
	    return val;
	}

	if ( isdigit((unsigned char)expr[pos]) )
	{
	    int64_t val = 0;
	    while ( pos < expr.size() && isdigit((unsigned char)expr[pos]) )
	    {
		val *= 10;
		val += expr[pos++] - '0';
	    }
	    // skip integer suffix (U, L, LL, ULL, etc.)
	    while ( pos < expr.size() && (expr[pos] == 'u' || expr[pos] == 'U'
		 || expr[pos] == 'l' || expr[pos] == 'L') )
		++pos;
	    return val;
	}

	if ( isalpha((unsigned char)expr[pos]) || expr[pos] == '_' )
	{
	    std::string word;
	    while ( pos < expr.size()
		 && (isalnum((unsigned char)expr[pos]) || expr[pos] == '_') )
		word += expr[pos++];

	    if ( word == "defined" )
	    {
		skip_ws();
		bool has_paren = false;
		if ( pos < expr.size() && expr[pos] == '(' )
		{
		    has_paren = true;
		    ++pos;
		    skip_ws();
		}
		std::string name;
		while ( pos < expr.size()
		     && (isalnum((unsigned char)expr[pos]) || expr[pos] == '_') )
		    name += expr[pos++];
		skip_ws();
		if ( has_paren && pos < expr.size() && expr[pos] == ')' )
		    ++pos;
		return macro_name_defined(name) ? 1 : 0;
	    }

	    // The clang __has_* family. Modern standard libraries gate whole
	    // implementation strategies on these — libc++ 18 asks __has_builtin
	    // 122 times and #errors outright for a trait it has no fallback for —
	    // and madc answered every one by falling through to "unknown
	    // identifier = 0". Now they are real operators.
	    if ( word.compare(0, 6, "__has_") == 0 )
		return evaluateHasQuery(word, expr, pos);

	    auto it = define_map.find(word);
	    if ( it != define_map.end() )
	    {
		if ( !it->empty() )
		    return strtoll(it->c_str(), NULL, 0);
		return 1;
	    }
	    if ( macro_map.count(word) > 0 )
		return 1;
	    return 0;
	}

	// skip single character like ',' or unknown
	++pos;
	return 0;
    };

    parse_unary = [&]() -> int64_t {
	skip_ws();
	if ( pos < expr.size() && expr[pos] == '!' )
	{
	    ++pos;
	    return parse_unary() ? 0 : 1;
	}
	if ( pos < expr.size() && expr[pos] == '~' )
	{
	    ++pos;
	    return ~parse_unary();
	}
	if ( pos < expr.size() && expr[pos] == '-'
	  && (pos + 1 < expr.size() && (isdigit((unsigned char)expr[pos+1])
		|| expr[pos+1] == '(' || isalpha((unsigned char)expr[pos+1]))) )
	{
	    ++pos;
	    return -parse_unary();
	}
	return parse_primary();
    };

    // Multiplicative operators (*, /, %)
    std::function<int64_t()> parse_multiplicative;
    parse_multiplicative = [&]() -> int64_t {
	int64_t value = parse_unary();
	for (;;)
	{
	    skip_ws();
	    if ( pos < expr.size() && expr[pos] == '*' )
	    {
		pos += 1;
		value *= parse_unary();
		continue;
	    }
	    if ( pos < expr.size() && expr[pos] == '/' )
	    {
		pos += 1;
		int64_t rhs = parse_unary();
		value = rhs ? value / rhs : 0;
		continue;
	    }
	    if ( pos < expr.size() && expr[pos] == '%' )
	    {
		pos += 1;
		int64_t rhs = parse_unary();
		value = rhs ? value % rhs : 0;
		continue;
	    }
	    return value;
	}
    };

    // Additive operators (+, -)
    std::function<int64_t()> parse_additive;
    parse_additive = [&]() -> int64_t {
	int64_t value = parse_multiplicative();
	for (;;)
	{
	    skip_ws();
	    if ( pos < expr.size() && expr[pos] == '+' )
	    {
		pos += 1;
		value += parse_multiplicative();
		continue;
	    }
	    if ( pos < expr.size() && expr[pos] == '-' )
	    {
		pos += 1;
		value -= parse_multiplicative();
		continue;
	    }
	    return value;
	}
    };

    // Shift operators (between additive and comparison in C precedence)
    std::function<int64_t()> parse_shift;
    parse_shift = [&]() -> int64_t {
	int64_t value = parse_additive();
	for (;;)
	{
	    skip_ws();
	    if ( pos + 1 < expr.size() && expr[pos] == '<' && expr[pos+1] == '<' )
	    {
		pos += 2;
		value <<= parse_unary();
		continue;
	    }
	    if ( pos + 1 < expr.size() && expr[pos] == '>' && expr[pos+1] == '>' )
	    {
		pos += 2;
		value >>= parse_unary();
		continue;
	    }
	    return value;
	}
    };

    parse_comparison = [&]() -> int64_t {
	int64_t value = parse_shift();
	for (;;)
	{
	    skip_ws();
	    if ( pos + 1 < expr.size() && expr[pos] == '=' && expr[pos+1] == '=' )
	    {
		pos += 2;
		value = (value == parse_shift()) ? 1 : 0;
		continue;
	    }
	    if ( pos + 1 < expr.size() && expr[pos] == '!' && expr[pos+1] == '=' )
	    {
		pos += 2;
		value = (value != parse_shift()) ? 1 : 0;
		continue;
	    }
	    if ( pos + 1 < expr.size() && expr[pos] == '<' && expr[pos+1] == '=' )
	    {
		pos += 2;
		value = (value <= parse_shift()) ? 1 : 0;
		continue;
	    }
	    if ( pos + 1 < expr.size() && expr[pos] == '>' && expr[pos+1] == '=' )
	    {
		pos += 2;
		value = (value >= parse_shift()) ? 1 : 0;
		continue;
	    }
	    if ( pos < expr.size() && expr[pos] == '<' && (pos + 1 >= expr.size() || expr[pos+1] != '<') )
	    {
		pos += 1;
		value = (value < parse_shift()) ? 1 : 0;
		continue;
	    }
	    if ( pos < expr.size() && expr[pos] == '>' && (pos + 1 >= expr.size() || expr[pos+1] != '>') )
	    {
		pos += 1;
		value = (value > parse_shift()) ? 1 : 0;
		continue;
	    }
	    return value;
	}
    };

    // Bitwise AND (&), XOR (^), OR (|) — between comparison and logical
    std::function<int64_t()> parse_bitand;
    std::function<int64_t()> parse_bitxor;
    std::function<int64_t()> parse_bitor;

    parse_bitand = [&]() -> int64_t {
	int64_t value = parse_comparison();
	for (;;)
	{
	    skip_ws();
	    // single & (not &&)
	    if ( pos < expr.size() && expr[pos] == '&'
	      && (pos + 1 >= expr.size() || expr[pos+1] != '&') )
	    {
		pos += 1;
		value &= parse_comparison();
		continue;
	    }
	    return value;
	}
    };

    parse_bitxor = [&]() -> int64_t {
	int64_t value = parse_bitand();
	for (;;)
	{
	    skip_ws();
	    if ( pos < expr.size() && expr[pos] == '^' )
	    {
		pos += 1;
		value ^= parse_bitand();
		continue;
	    }
	    return value;
	}
    };

    parse_bitor = [&]() -> int64_t {
	int64_t value = parse_bitxor();
	for (;;)
	{
	    skip_ws();
	    // single | (not ||)
	    if ( pos < expr.size() && expr[pos] == '|'
	      && (pos + 1 >= expr.size() || expr[pos+1] != '|') )
	    {
		pos += 1;
		value |= parse_bitxor();
		continue;
	    }
	    return value;
	}
    };

    parse_and = [&]() -> int64_t {
	int64_t value = parse_bitor();
	for (;;)
	{
	    skip_ws();
	    if ( pos + 1 < expr.size() && expr[pos] == '&' && expr[pos + 1] == '&' )
	    {
		pos += 2;
		int64_t rhs = parse_bitor();
		value = (value && rhs) ? 1 : 0;
		continue;
	    }
	    return value;
	}
    };

    parse_or = [&]() -> int64_t {
	int64_t value = parse_and();
	for (;;)
	{
	    skip_ws();
	    if ( pos + 1 < expr.size() && expr[pos] == '|' && expr[pos + 1] == '|' )
	    {
		pos += 2;
		int64_t rhs = parse_and();
		value = (value || rhs) ? 1 : 0;
		continue;
	    }
	    return value;
	}
    };

    // Conditional operator (?:) — the lowest #if precedence tier
    // (conditional-expression: logical-OR ? expression : conditional-expr,
    // right-associative). Without this tier the ternary's CONDITION value
    // leaked out as the result and both arms were ignored — glibc's
    // features.h `defined __cplusplus ? __cplusplus >= 201402L : defined
    // __USE_ISOC11` (__GLIBC_USE_DEPRECATED_GETS) took the wrong branch
    // under --std=c++11, hiding ::gets from <cstdio>'s using-declaration.
    parse_cond = [&]() -> int64_t {
	int64_t cond = parse_or();
	skip_ws();
	if ( pos < expr.size() && expr[pos] == '?' )
	{
	    ++pos;
	    int64_t then_v = parse_cond();
	    skip_ws();
	    if ( pos < expr.size() && expr[pos] == ':' )
		++pos;
	    int64_t else_v = parse_cond();
	    return cond ? then_v : else_v;
	}
	return cond;
    };

    return parse_cond() != 0;
}

TokenBase *Program::getToken()
{
    TokenBase *tb = _getToken();

    // Step 4: identifier spelling_id is now interned AT CREATION (make_ident and
    // the TokenIdent ctors), so the old getToken() stamp-from-str fallback is gone
    // along with the per-token `str`.
    DBG(if (tb) printt(tb));
    return tb;
}

// Discard the trailing content of a preprocessor directive line (after
// #endif/#else/#undef/#pragma/…) by running it THROUGH THE LEXER — the same
// path normal code takes — and stopping at the end-of-line token. This reuses
// the lexer's existing comment handling (`case '/'`, which reads a `/* */`
// block comment across physical newlines into one token), so a comment that
// opens on the directive line and continues onto the next is consumed in full
// instead of leaking its body as code (real <stddef.h>:
// `#endif /* … \n || __need_XXX was not defined before */`). Trailing tokens
// are dropped, matching GCC's "extra tokens at end of #endif directive". Only
// for ACTIVE directives — inactive #if branches are skipped at char level
// (skip_directive_line_tail), since their content must NOT be tokenized/expanded.
void Program::consume_directive_line_tail()
{
    TokenBase *t;
    while ( (t = getToken()) && t->type() != TokenType::ttEOL )
	/* discard — same as getRealToken() drops trivia */;
}

// The ONE pragma implementation, entered with `source` positioned at the pragma
// text itself (`pack(1)`, `push_macro("min")`, …). Both of the language's
// spellings reach it: the `#pragma` directive, which arrives already
// positioned, and the C99/C++11 `_Pragma("...")` operator, which destringizes
// its operand into the source stream first (handle_pragma_operator below).
// Staying text-driven is precisely what lets one implementation serve both — a
// pragma must behave identically however it was written, and pack / push_macro
// are the two madc genuinely acts on rather than ignores.
void Program::handle_pragma_body()
{
    while ( source.peek() == ' ' || source.peek() == '\t' )
	source.get();
    std::string pragma;
    int pragma_line = source.line();
    int pragma_col = source.column();
    while ( source.good() && !source.eof() && (isalpha(source.peek()) || source.peek() == '_') )
	pragma += source.get();
    if ( pragma == "pack" )
    {
	while ( source.peek() == ' ' || source.peek() == '\t' )
	    source.get();
	if ( source.peek() == '(' )
	{
	    source.get(); // consume (
	    while ( source.peek() == ' ' || source.peek() == '\t' )
		source.get();
	    std::string arg;
	    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
		arg += source.get();
	    // GCC semantics (see the pack block in madc.h):
	    // push saves the current value and an optional
	    // `, N` then sets it; pop restores; pack(N) sets;
	    // pack() resets to the default layout. Ops are
	    // QUEUED, not applied — parse-time application
	    // rides the token side channel (lex-time state
	    // would be stale by parse time).
	    if ( arg == "push" )
	    {
		while ( source.peek() == ' ' || source.peek() == '\t' || source.peek() == ',' )
		    source.get();
		int val = 0;
		while ( source.good() && isdigit(source.peek()) )
		    val = val * 10 + (source.get() - '0');
		_pending_pack_ops.push_back(std::make_pair(1, val));
		DBG(std::cout << "#pragma pack(push" << (val ? ", " : "")
		    << (val ? std::to_string(val) : std::string()) << ") queued" << std::endl);
	    }
	    else if ( arg == "pop" )
	    {
		_pending_pack_ops.push_back(std::make_pair(2, 0));
		DBG(std::cout << "#pragma pack(pop) queued" << std::endl);
	    }
	    else if ( !arg.empty() && isdigit((unsigned char)arg[0]) )
	    {
		_pending_pack_ops.push_back(std::make_pair(0, atoi(arg.c_str())));
		DBG(std::cout << "#pragma pack(" << arg << ") queued" << std::endl);
	    }
	    else if ( arg.empty() )
	    {
		_pending_pack_ops.push_back(std::make_pair(0, 0));
		DBG(std::cout << "#pragma pack() reset queued" << std::endl);
	    }
	    // discard trailing tokens via the lexer (handles a
	    // multi-line /* */ comment on this line)
	    consume_directive_line_tail();
	}
    }
    else if ( pragma == "push_macro" || pragma == "pop_macro" )
    {
	bool is_push = (pragma == "push_macro");
	while ( source.peek() == ' ' || source.peek() == '\t' )
	    source.get();
	if ( source.peek() == '(' )
	{
	    source.get(); // consume (
	    while ( source.peek() == ' ' || source.peek() == '\t' )
		source.get();
	    // Expect "macro_name"
	    if ( source.peek() == '"' )
	    {
		source.get(); // consume opening "
		std::string mname;
		while ( source.good() && !source.eof() && source.peek() != '"' )
		    mname += source.get();
		if ( source.peek() == '"' )
		    source.get(); // consume closing "
		if ( is_push )
		{
		    auto it = define_map.find(mname);
		    std::string val = (it != define_map.end()) ? *it : std::string("\x01");
		    _macro_save_stack[mname].push(val);
		    DBG(std::cout << "#pragma push_macro(\"" << mname << "\") saved=\"" << val << "\"" << std::endl);
		}
		else
		{
		    auto sit = _macro_save_stack.find(mname);
		    if ( sit != _macro_save_stack.end() && !sit->second.empty() )
		    {
			std::string val = sit->second.top();
			sit->second.pop();
			if ( val == "\x01" )
			    define_map.erase(mname);
			else
			    define_map[mname] = val;
			DBG(std::cout << "#pragma pop_macro(\"" << mname << "\") restored=\"" << val << "\"" << std::endl);
		    }
		}
	    }
	}
	// discard trailing tokens via the lexer (handles a
	// multi-line /* */ comment on this line)
	consume_directive_line_tail();
    }
    else if ( pragma == "prefer" )
    {
	std::vector<std::string> order;
	while ( source.peek() == ' ' || source.peek() == '\t' )
	    source.get();
	while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
	{
	    while ( source.peek() == ' ' || source.peek() == '\t' || source.peek() == ',' )
		source.get();
	    if ( source.peek() == '\n' || source.peek() == '\r' || source.eof() )
		break;
	    std::string name;
	    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
		name += source.get();
	    if ( !name.empty() )
	    {
		order.push_back(name);
		// The pragma reads its names char-by-char, so they bypass the
		// identifier lexer's auto-include scan — feed them to the same
		// hook the statement form gets for free, or `#pragma prefer
		// rust, ...` fails "Unknown namespace" while `prefer rust, ...;`
		// works (the ns_* header injects before parse-time validation).
		// positional=false: these names have no token-stream position,
		// so the stream-position gates must not read a stale one.
		auto_include_standard_identifier(name, /*positional=*/false);
	    }
	    while ( source.peek() == ' ' || source.peek() == '\t' )
		source.get();
	}

	TokenBase *tb = make_token(TokenID::tkPREFER);
	tb->line = pragma_line;
	tb->column = pragma_col;
	injected_tokens.push_back(tb);
	for ( size_t i = 0; i < order.size(); ++i )
	{
	    TokenIdent *ti = (TokenIdent *)make_ident(order[i]);
	    ti->line = pragma_line;
	    ti->column = pragma_col;
	    injected_tokens.push_back(ti);
	    if ( i + 1 < order.size() )
	    {
		tb = make_token(TokenID::tkComma);
		tb->line = pragma_line;
		tb->column = pragma_col;
		injected_tokens.push_back(tb);
	    }
	}
	tb = make_token(TokenID::tkSemi);
	tb->line = pragma_line;
	tb->column = pragma_col;
	injected_tokens.push_back(tb);
    }
    else
    {
	// consume the rest of the directive line for unknown
	// pragmas; discard trailing tokens via the lexer
	consume_directive_line_tail();
    }
}

// _Pragma("...") — the C99 (and C++11) token form of #pragma, which
// destringizes its operand and processes it as though it had been written as a
// directive. libc++ reaches it through _LIBCPP_SUPPRESS_DEPRECATED_PUSH and
// friends; it is not libc++-specific, though (doctest's macros use it too).
//
// The operand is read as a TOKEN rather than character-by-character because the
// standard macro-expands it first, and real headers depend on that: libc++
// writes both `_Pragma(#x)` and
// `_Pragma(_LIBCPP_TOSTRING(clang diagnostic ignored str))`, neither of which is
// a string literal until expansion has run. Going through the lexer gets that
// expansion for free — and the string case has already undone \" and \\, which
// `_Pragma("GCC diagnostic ignored \"-Wdeprecated\"")` needs — so the token's
// text IS the destringized pragma line, with no second unescaper to drift.
void Program::handle_pragma_operator()
{
    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t'
			   || source.peek() == '\n' || source.peek() == '\r') )
	source.get();
    if ( source.peek() != '(' )
	Throw << "_Pragma expects a parenthesized string literal" << flush;
    source.get();				// consume (
    TokenBase *operand = getRealToken();
    if ( !operand || operand->type() != TokenType::ttString )
	Throw << "_Pragma expects a parenthesized string literal" << flush;
    std::string text = ((TokenIdent *)operand)->spelling();
    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t'
			   || source.peek() == '\n' || source.peek() == '\r') )
	source.get();
    if ( source.peek() != ')' )
	Throw << "_Pragma: expected ')' after the pragma string" << flush;
    source.get();				// consume )
    DBG(std::cout << "_Pragma(\"" << text << "\")" << std::endl);
    // Hand the text back to the shared handler as a pragma line. The trailing
    // newline is load-bearing: every branch of handle_pragma_body ends by
    // discarding the rest of its directive line, so without a terminator that
    // discard would run off the end of the pushback and eat real code. A
    // pushed-back newline does not advance the line counter, so diagnostics
    // after a _Pragma still name the line it was written on.
    source.pushback(text + "\n");
    handle_pragma_body();
}

// Exact source text of a trivia token (whitespace via its RLE count, comment
// via its stored text incl. delimiters). Used for full-fidelity reconstruction.
static std::string trivia_text(TokenBase *tb)
{
    switch ( tb->type() )
    {
	case TokenType::ttSpace:   return std::string(((TokenSpace *)tb)->cnt, ' ');
	case TokenType::ttTab:     return std::string(((TokenTab *)tb)->cnt, '\t');
	case TokenType::ttEOL:     return std::string(((TokenEOL *)tb)->cnt, '\n');
	case TokenType::ttComment: return ((TokenREM *)tb)->str;
	default:                   return std::string();
    }
}

// get a real token (ignore whitespace and comments)
TokenBase *Program::getRealToken()
{
    TokenBase *tb;
    std::string pending_trivia;   // full-fidelity: leading whitespace/comments
    size_t synth_mark = source.synth_reads();

	while ( (tb=getToken()) )
	{
	    if ( source.synth_reads() != synth_mark )
	    {
		// Some byte of this read came from SYNTHESIZED pushback text
		// (macro expansion, __FILE__/__LINE__): the token's line and
		// column name the invocation site, not source bytes of its
		// own spelling. Coordinate consumers (parse_spans) skip it.
		tb->setFlag(tfSYNTHPOS);
		synth_mark = source.synth_reads();
	    }
	    if ( tb->line == 0 )
		tb->line = source.line(); //_line;
	    if ( tb->column == 0 )
		tb->column = source.column(); //_column;

	switch(tb->type())
	{
	    case TokenType::ttSpace:
	    case TokenType::ttTab:
	    case TokenType::ttEOL:
	    case TokenType::ttComment:
		if ( keep_trivia )
		    pending_trivia += trivia_text(tb);
		continue;
	    default:
		if ( keep_trivia && !pending_trivia.empty() )
		    tb->leading_trivia = std::move(pending_trivia);
		return tb;
	}
    }

    // EOF: stash any accumulated trailing trivia for reconstruct_source().
    if ( keep_trivia && !pending_trivia.empty() )
	_trailing_trivia = std::move(pending_trivia);
    return NULL;
}

// Best-effort source spelling of a lex-time token — the ONE token-spelling
// owner (declared in madc.h; reconstruct_source and the --emit=c++
// reverse-render both read it). NOTE: numeric literals store the parsed
// value, not the original text (0x1F -> "31"), and string escapes are not
// preserved — true byte-faithful reconstruction needs tokens to retain raw
// source text (a follow-on). Sufficient to demonstrate trivia retention on
// plain source. Keywords/identifiers/types/comments are all TokenIdent-derived.
std::string madc_token_spelling(TokenBase *tb)
{
    switch ( tb->type() )
    {
	case TokenType::ttString:
	    if ( TokenIdent *ti = dynamic_cast<TokenIdent *>(tb) )
	    {
		// Re-escape the cooked value so the literal RE-LEXES to the
		// same bytes (macro-arg re-lex, --dump-source round trips,
		// the --emit=c++ render recompiles) — the ONE escape rule.
		std::string sv = ti->spelling();
		return "\"" + madc_c_escape_string(sv.data(), sv.size())
		     + "\"";
	    }
	    return std::string();
	case TokenType::ttVariable:
	    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) ) return tv->var.name;
	    return std::string();
	case TokenType::ttInteger:
	{
	    TokenInt *ti = static_cast<TokenInt *>(tb);
	    return ti->source_text.empty() ? std::to_string(tb->ival())
					   : ti->source_text;
	}
	case TokenType::ttReal:
	{
	    TokenReal *tr = static_cast<TokenReal *>(tb);
	    return tr->source_text.empty() ? std::to_string(tb->dval())
					   : tr->source_text;
	}
	case TokenType::ttChar:
	{
	    // Reconstruct a char literal that re-lexes to the same value.
	    // TokenChar keeps only the value (not the original spelling), so this
	    // is a canonical form: printable ASCII emitted directly (the quote and
	    // backslash escaped), everything else as a hex escape. Without this case char
	    // literals reconstructed as empty (dropping `'\0'`/`'\x7f'` operands),
	    // which corrupted -E output fed back to the parser.
	    int v = (int)tb->ival();
	    if ( v >= 0x20 && v <= 0x7e && v != '\'' && v != '\\' )
		return std::string("'") + (char)v + "'";
	    char buf[16];
	    snprintf(buf, sizeof(buf), "'\\x%x'", (unsigned)(v & 0xff));
	    return std::string(buf);
	}
	case TokenType::ttOperator:
	case TokenType::ttSymbol:  return std::string(1, (char)tb->get());
	default:
	    if ( TokenIdent *ti = dynamic_cast<TokenIdent *>(tb) ) return ti->spelling();
	    if ( TokenMultiOp *to = dynamic_cast<TokenMultiOp *>(tb) ) return to->str;
	    return std::string();
    }
}

// THE token highlight classifier (declared in madc.h beside the spelling
// owner — madcide AST-2): presentation KIND by the token's lexed type.
// Keywords and datatypes are their own TokenType subtrees, so plain
// identifiers are what remains under tkIdent. Comments never reach the
// token stream (they are leading trivia) — the span query derives them.
HighlightClass madc_token_highlight_class(TokenBase *tb)
{
    switch ( tb->type() )
    {
	case TokenType::ttKeyword:  return HighlightClass::hcKeyword;
	case TokenType::ttDataType: return HighlightClass::hcType;
	case TokenType::ttInteger:
	case TokenType::ttReal:	    return HighlightClass::hcNumber;
	case TokenType::ttString:
	case TokenType::ttChar:	    return HighlightClass::hcString;
	default:
	    break;
    }
    if ( tb->id() == TokenID::tkIdent )
	return HighlightClass::hcIdent;
    return HighlightClass::hcNone;
}

// THE C-string-literal escape rule (declared in madc.h; dupaudit family
// c_string_literal_escape): the cooked bytes as a double-quoted literal's
// BODY. Canonical escapes; octal for non-printables (octal caps at 3
// digits — hex is maximal-munch and would swallow following hex digits).
// The token-spelling owner above and cir_emit_c's N_STR case both read it.
std::string madc_c_escape_string(const char *s, size_t len)
{
    std::string out;
    for ( size_t i = 0; s && i < len; ++i )
    {
	unsigned char c = (unsigned char)s[i];
	switch ( c )
	{
	    case '"':  out += "\\\""; break;
	    case '\\': out += "\\\\"; break;
	    case '\n': out += "\\n"; break;
	    case '\t': out += "\\t"; break;
	    case '\r': out += "\\r"; break;
	    default:
		if ( c >= 0x20 && c <= 0x7e )
		    out += (char)c;
		else
		{
		    char buf[8];
		    snprintf(buf, sizeof(buf), "\\%03o", c);
		    out += buf;
		}
	}
    }
    return out;
}

// Reconstruct source text from the token stream (full-fidelity mode): each
// token's leading trivia followed by its spelling. Requires keep_trivia to have
// been set before tokenizing.
std::string Program::reconstruct_source()
{
    std::string out;
    for ( TokenBase *tb : tokens )
    {
	out += tb->leading_trivia;
	out += madc_token_spelling(tb);
    }
    out += _trailing_trivia;   // whitespace/comments after the last token
    return out;
}

// print out a token with syntax highlighting, to debug parser
void Program::printt(TokenBase *tb)
{
    switch(tb->type())
    {
	case TokenType::ttSpace:
	    for ( int i = 0; i < ((TokenSpace *)tb)->cnt; ++i )
		std::cout << ' ';
	    break;
	case TokenType::ttTab:
	    for ( int i = 0; i < ((TokenTab *)tb)->cnt; ++i )
		std::cout << '\t';
	    break;
	case TokenType::ttEOL:
	    for ( int i = 0; i < ((TokenEOL *)tb)->cnt; ++i )
		std::cout << std::endl;
	    break;
	case TokenType::ttBase:
	    std::cout << "Got base token: " << (char)tb->get() << endl;
	    break;
	case TokenType::ttOperator:
	    if ( colors )
		std::cout << "\e[1;35m";
	    else
		std::cout << "OP::";
	    std::cout << (char)tb->get();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttMultiOp:
	    if ( colors )
		std::cout << "\e[1;35m";
	    else
		std::cout << "MOP::";
	    std::cout << ((TokenMultiOp *)tb)->str;
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttSymbol:
	    if ( colors )
		std::cout << "\e[1;36m";
	    else
		std::cout << "SY::";
	    std::cout << (char)tb->get();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttIdentifier:
	    if ( colors )
		std::cout << "\e[0;37m";
	    else
		std::cout << "ID::";
	    std::cout << ((TokenIdent *)tb)->spelling();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttVariable:
	    if ( colors )
		std::cout << "\e[0;37m";
	    else
		std::cout << "VAR::";
//	    std::cout << ((TokenVar *)tb)->var.name;
	    std::cout << dynamic_cast<TokenVar *>(tb)->var.name;
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttComment:
	    if ( colors )
		std::cout << "\e[1;32m";
	    else
		std::cout << "REM::";
	    std::cout << ((TokenIdent *)tb)->spelling();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttString:
	    if ( colors )
		std::cout << "\e[0;36m";
	    else
		std::cout << "STR::";
	    std::cout << '"' << ((TokenIdent *)tb)->spelling() << '"';
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttChar:
	    if ( colors )
	    {
		std::cout << "\e[0;32m'";
		std::cout << "\e[1;32m" << (char)tb->get();
		std::cout << "\e[0;32m'";
		std::cout << "\e[m";
		break;
	    }
	    std::cout << "CHAR::" << '\'' << (char)tb->get() << '\'';
	    break;
	case TokenType::ttInteger:
	    if ( colors )
		std::cout << "\e[0;33m";
	    else
		std::cout << "INT::";
	    std::cout << tb->get();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttReal:
	    if ( colors )
		std::cout << "\e[0;33m";
	    else
		std::cout << "REAL::";
	    std::cout << tb->get();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttKeyword:
	    if ( colors )
		std::cout << "\e[1;33m";
	    else
		std::cout << "KEY::";
	    std::cout << ((TokenIdent *)tb)->spelling();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttDataType:
	    if ( colors )
		std::cout << "\e[1;37m";
	    else
		std::cout << "TYPE::";
	    std::cout << ((TokenIdent *)tb)->spelling();
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	default:
	    std::cout << std::endl << "printt: Got unknown token (type: " << (int)tb->type() << "): " << (char)tb->get() << endl;
	    break;
    } // end switch
}

void Source::showerror(int row, int col)
{
//	std::cout << "showerror(" << row << ", " << col << ')' << std::endl;
	std::string ln;

	if ( !row || !col )
	{
	    row = line();
	    col = column();
	}

	// This display walk MUST be position-neutral: a WARNING resumes lexing
	// right after it prints, so the cursor state is saved here and restored
	// before every return. (It was destructive-only before — every caller
	// was a fatal error path, so the clobbered cursor never mattered.)
	size_t saved_gpos = _gpos;
	int saved_cr = _cr, saved_lf = _lf, saved_column = _column;

	// Flat-buffer rewind: reset the read cursor to the start and re-scan
	// forward to the error line (was _ss.seekg(0) on the old stringstream).
	_cr = _lf = 0;
	_gpos = 0;

	while ( peek() != -1 )
	{
	    getline(ln);
	    //cout << "line()-1 " << (line()-1) << "  row " << row << endl;
	    if ( line()-1 >= row )
		break;
        }

	_gpos = saved_gpos;
	_cr = saved_cr; _lf = saved_lf; _column = saved_column;

	show_error_source_line(ln, col);
}

// Shared display tail for a diagnostic source echo: the offending line and a
// caret under the column, truncated to the terminal width — one formatter for
// both the live-Source echo and the reread-from-disk echo.
void show_error_source_line(const std::string &ln, int col)
{
    char *env_columns = getenv("COLUMNS");
    size_t term_columns = env_columns ? (size_t)atoi(env_columns) : 80;

    if ( ln.length()+5 > term_columns )
    {
	// The column can exceed the fetched line (a token inside a macro
	// expansion carries post-expansion provenance). A diagnostic must
	// never throw — clamp the tail slice to the line's end instead of
	// letting substr raise out_of_range mid-print (which surfaced as
	// "tree build failed (basic_string::substr...)" and MASKED the
	// real error).
	size_t start = (col > 0 && (size_t)col <= ln.length())
		     ? (size_t)col : ln.length();
	std::string trunc = "  ..." + ln.substr(start);
	std::cerr << trunc << std::endl;
	std::cerr << std::setw(4) << ' ' << "\e[1;32m^\e[m" << std::endl;
	return;
    }
    std::cerr << ln << std::endl;
    if ( col > 1 )
	std::cerr << std::setw(col-1) << ' ';
    std::cerr << "\e[1;32m^\e[m" << std::endl;
}

// Echo line `row` of a file that is NOT the live Source buffer — a token from
// an #included file. Diagnostics are a cold path, so reread from disk. Returns
// false (echo skipped) when the file cannot be opened or is shorter than
// `row` — e.g. an embedded header with no on-disk presence, or stale
// provenance; skipping beats echoing the wrong file's text.
bool madc_show_file_error(const char *fname, int row, int col)
{
    if ( !fname || !*fname || row <= 0 )
	return false;
    std::ifstream f(fname);
    if ( !f.is_open() )
	return false;
    std::string ln;
    int i = 0;
    while ( i < row && std::getline(f, ln) )
	++i;
    if ( i != row )
	return false;
    show_error_source_line(ln, col);
    return true;
}

int throwbuf::sync()
{
    if ( DiagnosticRenderMute::active )
	throw std::exception();	// captured as data — render nothing
    cerr << endl;
    if ( _tb )
    {
	// file, line, column, AND the echoed source line must all come from
	// the SAME provenance — the token's. Before this, an error inside an
	// #included file printed the top-level file's NAME with the header's
	// LINE and echoed the top-level file's text (three-way inconsistent).
	const char *tok_file = (_tb->file && *_tb->file) ? _tb->file : NULL;
	const char *fname = tok_file ? tok_file
			  : (_src ? _src->fname() : "???");
	cerr << ANSI_WHITE << fname << ':' << _tb->line << ':' << _tb->column
	     << ": \e[1;31merror:\e[1;37m " << str() << ANSI_RESET << endl;
	if ( _src && (!tok_file || strcmp(tok_file, _src->fname()) == 0) )
	    _src->showerror(_tb->line, _tb->column);
	else
	    madc_show_file_error(fname, _tb->line, _tb->column);
    }
    else
    if ( _src )
    {
	cerr << ANSI_WHITE << _src->fname() << ':' << _src->line() << ':' << _src->column()
	     << ": \e[1;31merror:\e[1;37m " << str() << ANSI_RESET << endl;
	_src->showerror();
    }
    else
    {
	cerr << ANSI_WHITE << ": \e[1;31merror:\e[1;37m " << str() << ANSI_RESET << endl;
    }
    throw std::exception();
    return -1;
}


#if 0
void Program::showerror(istream &is)
{
    char *env_columns = getenv("COLUMNS");
    string line;
    size_t term_columns;

    if ( env_columns )
	term_columns = atoi(env_columns);
    else
	term_columns = 80;

    is.clear();
    is.seekg(_pos, is.beg);
    if ( !is.good() )
	cerr << " seekfail";
    getline(is, line);
    if ( line.length()+5 > term_columns )
    {
	line = "  ..." + line.substr(_column);
	cerr << line << endl;
	cerr << setw(4) << ' ' << "\e[1;32m^\e[m" << endl;
	return;
    }
    cerr << line << endl;
    cerr << setw(_column-1) << ' ' << "\e[1;32m^\e[m" << endl;
}
#endif

#if 0
// tokenize stream of data TODO -- do all the same as tokenize(file), except set up filename
void Program::tokenize(istream &ss)
{
    TokenBase *tb = NULL;

    DBG(std::cout << "Program::parse()" << std::endl << std::endl);

    _init();

    while ( (tb=getToken()) )
	parseStatement(tb);

    DBG(std::cout << "Program::parse() finished parsing" << std::endl);

    _finalize();
}
#endif


// tokenize a file
TokenProgram *Program::tokenize(const char *fname)
{
    TokenBase *tb;
    ifstream file(fname);

    DBG(cout << "Program::tokenize(" << fname << ") START" << endl);
    clear_diagnostics();
    clear_error();

    if ( !file )
    {
	record_frontend_error(Program::DiagnosticPhase::lexer,
			      "Failed to open file", fname, 0, 0);
	return NULL;
    }

    _tokenizer_init();

    // INTERN the caller's name before anything stores it: every main-source
    // token's `file` and forest_root_file keep this pointer for the
    // Program's LIFETIME, while the caller's buffer need not outlive the
    // call — libmadc's with_temp_source frees its temp path right after
    // compiling, and the lazy JIT build later walked freed memory
    // (valgrind: invalid read in is_system_header_path from
    // translate_module; two layout-dependent libmadc unit asserts). The
    // buffer lane (tokenize_buffer) already interned; this lane is the
    // same rule.
    fname = intern_file(fname);

    forest_root_file = fname;	// v24: the program's own file — its state
				// never enters the forest's bind surface
    source.fname(fname);
    {
	ReadTimer _rt(_read_seconds);
	file.seekg(0, std::ios::end);	// --show-stats: main source-file bytes
	if ( file.tellg() > 0 ) _input_bytes += (size_t)file.tellg();
	file.seekg(0);
	source.copybuf(file.rdbuf());
    }
    pack_note_unit(pack_recording ? intern_file(fname) : NULL);	// B4a: main unit first
    Throw.source(source);

    try
    {
	// Stage-2: yield every LEX_YIELD_GRAIN tokens (the token pump's
	// chunk grain — sub-millisecond slices; the check is one counter
	// compare, and parse_yield_point itself no-ops without tasks).
	uint32_t pump = 0;
	while ( (tb=getRealToken()) )
	{
	    tb->file = fname;
//	    tb->line = source.line();
//	    tb->column = source.column();
	    push_token_with_literal_concat(tb);
	    if ( (++pump & (LEX_YIELD_GRAIN - 1)) == 0 )
	    {
		parse_yield_point();
		lex_abort_if_task_cancelled();	// MT-3b: clean abort
	    }
        }
    }
    catch(const char *err_msg)
    {
	record_frontend_error(Program::DiagnosticPhase::lexer,
			      err_msg ? err_msg : "(null error message)",
			      fname, source.line(), source.column());
	return NULL;
    }
    catch(TokenIdent *ti)
    {
	record_frontend_error(Program::DiagnosticPhase::lexer,
			      std::string("use of undeclared identifier '")
				  + ti->spelling() + '\'',
			      fname, source.line(), source.column());
	return NULL;
    }
    catch(TokenBase *tb)
    {
	record_frontend_error(Program::DiagnosticPhase::lexer,
			      std::string("unexpected token type ")
				  + std::to_string((int)tb->type()),
			      fname, source.line(), source.column());
	return NULL;
    }
    catch(std::exception &e)
    {
	if ( !last_error.has_error )
	    record_throw_diagnostic(e, Program::DiagnosticPhase::lexer,
				    fname, source.line(), source.column());
	print_unrendered_diagnostic();
	return NULL;
    }

    DBG(std::cout << "Program::tokenize() finished tokenizing" << std::endl);

    // Auto-includes inject BEFORE the forest flush: the synthetic #include
    // must bind its frozen unit while the one-shot decl restore (item 5,
    // inside the flush) can still see it in forest_chain_set. The parse()-
    // start injection stays as the live/eval fallback (pending set is empty
    // here after this call), but under a forest bind it ran too late — the
    // restore window had closed and the auto-included header's names never
    // registered (`string s` in a bare script: "use of undeclared
    // identifier 'string'" only when packed).
    inject_pending_auto_includes();

    tkProgram = new TokenProgram();
    tkFunction = tkProgram;
    flush_forest_pending_globals();	// v13: globals staged during #include bind

    file.clear();

    tkProgram->source = fname;
    tkProgram->is = new ifstream(fname);
    tkProgram->lines = source.line()-1;
    tkProgram->bytes = file.tellg();

    return tkProgram;
}

TokenProgram *Program::tokenize_buffer(const std::string &source_text,
				       const std::string &display_name)
{
    TokenBase *tb;
    std::string effective_name = display_name.empty() ? "<memory>" : display_name;
    const char *fname = intern_file(effective_name);

    DBG(cout << "Program::tokenize_buffer(" << effective_name << ") START" << endl);
    clear_diagnostics();
    clear_error();

    _tokenizer_init();

    forest_root_file = fname;	// v24 (see tokenize)
    source.fname(fname);
    { ReadTimer _rt(_read_seconds); source.str(source_text); }
    _input_bytes += source_text.size();	// --show-stats: load_buffer main-source bytes
    pack_note_unit(pack_recording ? fname : NULL);	// B4a: main unit first
    Throw.source(source);

    try
    {
	// Stage-2: the same yield grain as tokenize() — this is the pump
	// the IDE's parse handles ride (parse_open -> tokenize_buffer).
	uint32_t pump = 0;
	while ( (tb=getRealToken()) )
	{
	    tb->file = fname;
	    push_token_with_literal_concat(tb);
	    if ( (++pump & (LEX_YIELD_GRAIN - 1)) == 0 )
	    {
		parse_yield_point();
		lex_abort_if_task_cancelled();	// MT-3b: clean abort
	    }
	}
    }
    catch(const char *err_msg)
    {
	record_frontend_error(Program::DiagnosticPhase::lexer,
			      err_msg ? err_msg : "(null error message)",
			      fname, source.line(), source.column());
	return NULL;
    }
    catch(TokenIdent *ti)
    {
	record_frontend_error(Program::DiagnosticPhase::lexer,
			      std::string("use of undeclared identifier '")
				  + ti->spelling() + '\'',
			      fname, source.line(), source.column());
	return NULL;
    }
    catch(TokenBase *tb)
    {
	record_frontend_error(Program::DiagnosticPhase::lexer,
			      std::string("unexpected token type ")
				  + std::to_string((int)tb->type()),
			      fname, source.line(), source.column());
	return NULL;
    }
    catch(std::exception &e)
    {
	if ( !last_error.has_error )
	    record_throw_diagnostic(e, Program::DiagnosticPhase::lexer,
				    fname, source.line(), source.column());
	print_unrendered_diagnostic();
	return NULL;
    }

    DBG(std::cout << "Program::tokenize_buffer() finished tokenizing" << std::endl);

    // Auto-includes inject BEFORE the forest flush — see tokenize()'s tail.
    inject_pending_auto_includes();

    tkProgram = new TokenProgram();
    tkFunction = tkProgram;
    flush_forest_pending_globals();	// v13: globals staged during #include bind

    tkProgram->source = effective_name;
    tkProgram->is = new std::stringstream(source_text);
    tkProgram->lines = source.line()-1;
    tkProgram->bytes = source_text.size();

    return tkProgram;
}
