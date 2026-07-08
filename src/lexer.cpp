//////////////////////////////////////////////////////////////////////////
//									//
// madc lexer methods to tokenize a source file into tokens		//
//									//
//////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dlfcn.h>
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
#include "madc_pch.h"
#include "cir_freeze.h"	// Phase 6: CirFrozenForest — parse-time grove binding

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

using namespace std;

static bool find_filesystem_precompiled_header(Program &pgm,
					       const std::string &incfile,
					       bool is_system,
					       std::string &outpath);
static bool load_precompiled_header_file(const std::string &path,
					 std::deque<TokenBase *> &tokens);
static bool push_precompiled_header_tokens(Program &pgm,
					   const std::string &display_name,
					   std::deque<TokenBase *> &pch_tokens);

static DataDef *get_complex_compat_type(DataDef *base_type)
{
    static std::map<DataDef *, DataDefCOMPLEX *> cache;
    if ( !base_type )
	base_type = &ddDOUBLE;
    std::map<DataDef *, DataDefCOMPLEX *>::iterator it = cache.find(base_type);
    if ( it != cache.end() )
	return it->second;
    DataDefCOMPLEX *complex_type = new DataDefCOMPLEX(*base_type);
    cache[base_type] = complex_type;
    return complex_type;
}

static bool builtin_alias_needs_retokenize(const std::string &word,
					   const std::string &val)
{
    (void)val;
    return word == "__builtin_va_list";
}

static bool is_prefixed_literal_token(const std::string &ident,
				      const std::string &body,
				      size_t pos)
{
    if ( pos >= body.size() )
	return false;
    char next = body[pos];
    if ( next != '\'' && next != '"' )
	return false;
    return ident == "L" || ident == "u" || ident == "U" || ident == "u8";
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

static std::string narrow_string_as_wide(const std::string &narrow)
{
    std::string out;
    append_narrow_string_as_wide(out, narrow);
    return out;
}

TokenBase *Program::read_wide_literal()
{
    char quote = source.get();
    int row = source.line();
    int col = source.column();
    if ( quote == '"' )
    {
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
	    append_wide_codepoint(bytes, cp);
	}
	if ( !source.good() )
	{
	    source.setpos(row, col);
	    throw "Unterminated wide string";
	}
	source.get();
	return make_str(bytes, true);
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
    ti->setDataType(&ddINT32);
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

static bool next_source_word_is(Source &source, const char *match)
{
    std::string consumed;
    while ( source.good() )
    {
	int c = source.peek();
	if ( c == ' ' || c == '\t' || c == '\n' || c == '\r'
	  || c == '\f' || c == '\v' )
	    consumed += (char)source.get();
	else
	    break;
    }
    std::string word;
    while ( source.good() )
    {
	int c = source.peek();
	if ( isalnum((unsigned char)c) || c == '_' )
	{
	    word += (char)source.get();
	    consumed += word.back();
	}
	else
	    break;
    }
    if ( !consumed.empty() )
	source.pushback_reread(consumed);
    return word == match;
}

static bool identifier_matches_gnu_attribute_name(const std::string &id,
						  const std::string &name)
{
    return id == name || id == "__" + name + "__";
}

static bool gnu_attribute_text_has_name(const std::string &text,
					const std::string &name)
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
	if ( identifier_matches_gnu_attribute_name(id, name) )
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

static const char *auto_include_embedded_header_for_identifier(const std::string &word)
{
    static const std::map<std::string, std::string> identifier_headers = {
	{"string", "string"},
	{"stringstream", "sstream"},

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
	"vector",
	"map",
	"set",
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
					   const char *source_name)
{
    if ( !source_name || !*source_name )
	return 0;
    for ( size_t i = 0; i < limit; ++i )
    {
	TokenBase *tb = tokens[i];
	if ( tb && tb->file && strcmp(tb->file, source_name) == 0 )
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
}

bool Program::auto_include_standard_identifier(const std::string &word)
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
    for ( auto it = tokens.rbegin(); it != tokens.rend(); ++it )
    {
	TokenBase *t = *it;
	TokenID tid = t->id();
	TokenType tt = t->type();
	if ( tid == TokenID::tkMul )
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
	break;
    }

    const char *header = auto_include_embedded_header_for_identifier(word);
    if ( !header )
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
						    source.fname());

    while ( !pending_auto_include_headers.empty() )
    {
	std::set<std::string> batch;
	batch.swap(pending_auto_include_headers);
	std::vector<std::string> ordered = ordered_auto_include_headers(batch);
	for ( std::vector<std::string>::const_iterator hi = ordered.begin();
	      hi != ordered.end(); ++hi )
	{
	    const std::string &header = *hi;
	    std::string include_key = std::string("<") + header + ">";
	    if ( !should_tokenize_include(include_key) )
		continue;

	    std::string pch_path;
	    if ( find_filesystem_precompiled_header(*this, header, true, pch_path) )
	    {
		std::deque<TokenBase *> pch_tokens;
		if ( load_precompiled_header_file(pch_path, pch_tokens) )
		{
		    push_precompiled_header_tokens(*this, pch_path, pch_tokens);
		    continue;
		}
	    }
	    const PrecompiledHeader *pch = find_precompiled_header(header);
	    if ( pch )
	    {
		std::deque<TokenBase *> pch_tokens;
		if ( madc_pch::read_madh(pch->data, pch->size, pch_tokens) )
		{
		    push_precompiled_header_tokens(*this, header, pch_tokens);
		    continue;
		}
	    }

	    const std::string *embedded = find_embedded_header(header);
	    if ( !embedded )
		continue;
	    if ( !is_embedded_header_allowed(header) )
		Throw << "embedded header '" << header
		      << "' is not allowed by registration policy" << flush;

	    pack_record_edge(header);	// B4a: auto-include edge, pre-swap
	    Source saved = std::move(source);
	    bool saved_suppress_auto_include_scan = suppress_auto_include_scan;
	    suppress_auto_include_scan = true;
	    source = Source();
	    source.fname(header.c_str());
	    { ReadTimer _rt(_read_seconds); source.str(*embedded); }
	    _input_bytes += embedded->size();	// --show-stats: embedded header bytes
	    TokenBase *itb;
	    const char *interned = intern_file(header);
	    pack_note_unit(interned);
	    while ( (itb = getRealToken()) )
	    {
		itb->file = interned;
		push_token_with_literal_concat(itb);
	    }
	    source = std::move(saved);
	    suppress_auto_include_scan = saved_suppress_auto_include_scan;
	    mark_embedded_include_flag(header);
	}
    }

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

    for ( const std::string &arg : args )
    {
	size_t i = 0;
	while ( i < arg.size() && (arg[i] == ' ' || arg[i] == '\t') )
	    ++i;
	if ( i >= arg.size() )
	    continue;
	if ( arg.compare(i, 3, "...") == 0 )
	    return true;
	if ( starts_with_type_word(arg) )
	    return true;
    }
    return false;
}

static std::string read_macro_body(Source &source)
{
    std::string body;

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
		body += ' ';
		continue;
	    }
	    body += '\\';
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
	// the merged literal lives in `prev` (already in the stream); refresh its
	// rec spelling to the concatenated bytes so its ROM stays self-describing.
	prev->rec.spelling_id = strpool.intern(prev->str);
	delete tb;
	return;
    }
    ++_tok_produced;	// --show-stats: a real stream token emitted by the lexer
    finalize_pop1_rec(tb);
    tokens.push_back(tb);
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
    TokenBase::_active_strpool = &strpool;
    TokenBase::_active_valpool = &valpool;	// P0 slice 3: wide constants resolve via wival()
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
    deferred_function_body_sink = NULL;
    parsing_cpp_struct_class = false;
    _include_iostream = false;
    _include_stdio = false;
    _include_string = false;
    included_files.clear();
    include_guard_by_file.clear();
    pending_auto_include_headers.clear();
    pending_auto_include_identifiers.clear();
    suppress_auto_include_scan = false;
    pending_no_strict_aliasing = false;
    add_keywords();
    add_datatypes();

    // Ignored C storage hints. Type qualifiers are real tokens so
    // macro token-pasting can still see their spelling.
    define_map["inline"] = "";
    define_map["__inline__"] = "";
    define_map["__inline"] = "";
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
    define_map["__builtin_va_list"] = "long";
    define_map["__builtin_va_arg"] = "va_arg";
    define_map["__null"] = "0";
    // __builtin_va_start passes through to the CIR as a real c2mir intrinsic
    // call (cir_builder emits N_CALL(__builtin_va_start, ap); c2mir lowers it to
    // MIR_VA_START on the user's own va_list). It is intentionally NOT a macro:
    // the earlier `__ap = __va_args` master-copy expansion mis-set reg_save_area
    // in large frames. va_end stays a no-op (the stdarg.h va_end macro handles
    // it as `((void)(ap))`).
    {
	MacroDef m;
	m.params = {"__ap"};
	m.body = "__ap = 0";
	macro_map["__builtin_va_end"] = m;
    }
    {
	MacroDef m;
	m.params = {"__dest", "__src"};
	m.body = "__dest = __src";
	macro_map["__builtin_va_copy"] = m;
    }
    // Report the GCC version that compiled madc itself.
    define_map["__GNUC__"] = std::to_string(__GNUC__);
    define_map["__GNUC_MINOR__"] = std::to_string(__GNUC_MINOR__);
    define_map["__GNUC_PATCHLEVEL__"] = std::to_string(__GNUC_PATCHLEVEL__);
    define_map["__x86_64__"] = "1";
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
    define_map["__builtin_fabs"] = "fabs";
    define_map["__builtin_fabsf"] = "fabsf";
    define_map["__builtin_fabsl"] = "fabsl";
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
    define_map["__builtin_copysign"] = "copysign";
    define_map["__builtin_copysignf"] = "copysignf";
    define_map["__builtin_copysignl"] = "copysignl";
    define_map["__builtin_sqrtf"] = "sqrtf";
    define_map["__builtin_sqrt"] = "sqrt";
    define_map["__builtin_sqrtl"] = "sqrtl";
    define_map["__builtin_logf"] = "logf";
    define_map["__builtin_log"] = "log";
    define_map["__builtin_expf"] = "expf";
    define_map["__builtin_exp"] = "exp";
    define_map["__builtin_sinf"] = "sinf";
    define_map["__builtin_sin"] = "sin";
    define_map["__builtin_cosf"] = "cosf";
    define_map["__builtin_cos"] = "cos";
    define_map["__builtin_floorf"] = "floorf";
    define_map["__builtin_floor"] = "floor";
    define_map["__builtin_ceilf"] = "ceilf";
    define_map["__builtin_ceil"] = "ceil";
    define_map["__builtin_roundf"] = "roundf";
    define_map["__builtin_round"] = "round";
    define_map["__builtin_fmaxf"] = "fmaxf";
    define_map["__builtin_fmax"] = "fmax";
    define_map["__builtin_fmaxl"] = "fmaxl";
    define_map["__builtin_powf"] = "powf";
    define_map["__builtin_pow"] = "pow";
    define_map["__builtin_fmaf"] = "fmaf";
    define_map["__builtin_fma"] = "fma";
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
    // __builtin_classify_type(x) → 0 (integer type, simplified)
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "0";
	macro_map["__builtin_classify_type"] = m;
    }
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

    // __builtin_classify_type(x) — return 0 (void type) as placeholder
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "0";
	macro_map["__builtin_classify_type"] = m;
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
	define_map[d.first] = d.second;
}

bool Program::include_already_seen(const std::string &path)
{
    return included_files.count(path) != 0;
}

std::string Program::current_source_directory()
{
    std::string cur_fname(source.fname());
    size_t slash_pos = cur_fname.rfind('/');
    if ( slash_pos == std::string::npos )
	return "";
    return cur_fname.substr(0, slash_pos + 1);
}

// Phase 6 (--forest-bind): open the grove container once, on the first system
// #include. The container source is forest_bind_path (a standalone --freeze
// container) or the blob appended to this executable (empty -> /proc/self/exe).
// A missing/mismatched container leaves bind_forest NULL: every include then
// live-parses. The mapping is never unmapped — the forest reads from it for the
// process lifetime.
CirFrozenForest *Program::ensure_bind_forest()
{
    if ( bind_forest_tried )
	return bind_forest;
    bind_forest_tried = true;
    const void *image = NULL;
    size_t image_len = 0;
    const char *path = forest_bind_path.empty() ? NULL : forest_bind_path.c_str();
    if ( !cir_forest_map_image(path, image, image_len) )
    {
	DBG(std::cout << "forest-bind: no container at "
	    << (path ? path : "/proc/self/exe") << " — live parse" << std::endl);
	return NULL;
    }
    CirFrozenForest *f = new CirFrozenForest();
    if ( !f->open(image, image_len, /*c2m=*/NULL) )
    {
	// open() already printed the reason (pin mismatch / corrupt directory).
	delete f;
	return NULL;
    }
    bind_forest = f;
    DBG(std::cout << "forest-bind: opened container (" << f->unit_count()
	<< " units)" << std::endl);
    return bind_forest;
}

// Map a system include spelling to a frozen grove unit index (-1 = miss). Tries
// the bare spelling first (compiler-builtin/embedded units name themselves, e.g.
// "stddef.h"), then the resolved filesystem path (real headers name their full
// path) — the same resolution the live path would use, so a hit binds the exact
// grove the pack froze.
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
    return u;
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
    if ( forest_chain_set.count(unit) || forest_bind_walking.count(unit) )
	return;
    forest_bind_walking.insert(unit);
    std::vector<uint32_t> edges;
    if ( bind_forest->unit_edges(unit, edges) )
	for ( size_t i = 0; i < edges.size(); ++i )
	    forest_bind_include(edges[i]);
    forest_install_pp(unit);
    forest_chain.push_back(unit);
    forest_chain_set.insert(unit);
    forest_bind_walking.erase(unit);
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
    for ( size_t k = 0; k + 5 <= ev.size(); )
    {
	uint32_t name_id = ev[k], tag_flags = ev[k + 1], body_id = ev[k + 2];
	uint32_t vpar_id = ev[k + 3], nparams = ev[k + 4];
	const char *nm = bind_forest->pool_str(name_id);
	uint8_t tag = (uint8_t)(tag_flags & 0xff);
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
	// System include paths captured at BUILD time from the configured
	// compiler's own search list (scripts/gen_sys_includes.sh ->
	// src/sys_include_paths.cpp) — so madc searches the SAME dirs the toolchain
	// does, including the C++ paths (/usr/include/c++/NN, …) the old hardcoded
	// C-only list lacked. Falls back to that minimal C list when detection
	// produced nothing (no compiler at build time).
	extern const char *madc_sys_include_paths[];
	static const char *fallback_paths[] = {
	    "/usr/local/include/",
	    "/usr/include/",
	    "/usr/include/x86_64-linux-gnu/",
	    NULL
	};
	const char **sys_paths = (madc_sys_include_paths[0] != NULL)
				 ? madc_sys_include_paths : fallback_paths;
	for ( int i = 0; sys_paths[i]; ++i )
	{
	    std::string candidate = std::string(sys_paths[i]) + incfile;
	    std::ifstream probe(candidate.c_str());
	    if ( probe.good() )
		return candidate;
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
	return incfile; // not found — will fail at open
    }

    // "file.h": current source directory, then -I paths
    std::string cur_dir = current_source_directory();
    if ( !cur_dir.empty() )
    {
	std::string local = cur_dir + incfile;
	std::ifstream probe(local.c_str());
	if ( probe.good() )
	    return local;
    }
    for ( size_t i = 0; i < include_paths.size(); ++i )
    {
	std::string &dir = include_paths[i];
	std::string candidate = dir + (dir.empty() || dir.back() == '/' ? "" : "/") + incfile;
	std::ifstream probe(candidate.c_str());
	if ( probe.good() )
	    return candidate;
    }
    return cur_dir.empty() ? incfile : cur_dir + incfile;
}

// Is this source file one that came from a system/toolchain include directory
// (glibc / libstdc++ / their bits/), as opposed to the user's own .mad/.c/.h?
// Data-driven (project Rule #7): a prefix match against the SAME compiler-derived
// search dirs `#include <...>` uses (madc_sys_include_paths[], generated by
// scripts/gen_sys_includes.sh) — never a namespace==std or class-name test.
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
    extern const char *madc_sys_include_paths[];
    for ( int i = 0; madc_sys_include_paths[i]; ++i )
    {
	const char *prefix = madc_sys_include_paths[i];
	size_t plen = strlen(prefix);
	if ( plen && strncmp(path, prefix, plen) == 0 )
	    return true;
    }
    return false;
}

// #include_next <file>: behave like a system #include, but search the
// path list starting AFTER the directory the current file was found in.
// Used by libstdc++ wrapper headers (cstdlib -> stdlib.h, cmath -> math.h …)
// to reach the "real" header of the same name that sits later in the search
// order. The embedded/curated layer is consulted first by the caller (so
// curated libc headers still win while libc stays curated); this resolves the
// filesystem fallback for the non-curated targets.
std::string Program::resolve_include_next_path(const std::string &incfile)
{
    if ( incfile.empty() || incfile[0] == '/' )
	return incfile;

    // Ordered system search list — the same dirs <> includes search:
    // -I paths first, then the compiler-derived system include paths.
    std::vector<std::string> search;
    for ( size_t i = 0; i < include_paths.size(); ++i )
	search.push_back(include_paths[i]);
    extern const char *madc_sys_include_paths[];
    static const char *fallback_paths[] = {
	"/usr/local/include/",
	"/usr/include/",
	"/usr/include/x86_64-linux-gnu/",
	NULL
    };
    const char **sys_paths = (madc_sys_include_paths[0] != NULL)
			     ? madc_sys_include_paths : fallback_paths;
    for ( int i = 0; sys_paths[i]; ++i )
	search.push_back(sys_paths[i]);

    // Normalize-with-trailing-slash compare to locate the current file's dir
    // in the search list; #include_next searches only entries AFTER it.
    std::string cur = current_source_directory();
    if ( !cur.empty() && cur.back() != '/' )
	cur += '/';
    size_t start = 0;
    for ( size_t i = 0; i < search.size(); ++i )
    {
	std::string d = search[i];
	if ( !d.empty() && d.back() != '/' )
	    d += '/';
	if ( d == cur )
	{
	    start = i + 1;
	    break;
	}
    }
    for ( size_t i = start; i < search.size(); ++i )
    {
	std::string &dir = search[i];
	std::string candidate = dir + (dir.empty() || dir.back() == '/' ? "" : "/") + incfile;
	std::ifstream probe(candidate.c_str());
	if ( probe.good() )
	    return candidate;
    }
    return incfile; // not found — will fail at open
}

// Detect the classic include guard of a header file: the first significant
// line is `#ifndef NAME` (or `#if !defined(NAME)`) and its matching `#endif`
// closes the file with nothing significant after it. Returns the guard macro
// name, or "" when the file is NOT fully guard-wrapped (e.g. glibc's
// bits/mathcalls.h, which is INTENTIONALLY included multiple times with a
// different _Mdouble_ each pass).
static std::string detect_include_guard(const std::string &file_path)
{
    std::ifstream in(file_path.c_str());
    if ( !in )
	return std::string();
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
    const char *f = source.fname();
    if ( !f || !*f )
	return NULL;
    return intern_file(f);
}

void Program::pack_note_unit(const char *interned_file)
{
    if ( !pack_recording || !interned_file )
	return;
    if ( pack_units_seen.insert(interned_file).second )
	pack_unit_order.push_back(interned_file);
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

void Program::pack_record_branch_macro(const std::string &name)
{
    if ( pack_recording && !name.empty() )
	pack_branch_macros.insert(name);
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
    std::string canonical = path;
    if ( !path.empty() && path[0] != '<' )
    {
	char *rp = realpath(path.c_str(), NULL);
	if ( rp )
	{
	    canonical = rp;
	    free(rp);
	}
    }
    if ( !path.empty() && path[0] == '<' )
    {
	// Named (embedded/PCH) include keys: blanket once-only — the baked
	// sets assume single inclusion and the surviving embedded headers
	// carry no #ifndef guards of their own.
	if ( include_already_seen(canonical) )
	    return false;
	included_files[canonical] = true;
	return true;
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
	return true;
    }
    // System headers: gcc's multiple-include optimization. Skip a
    // repeat inclusion ONLY when the file is fully wrapped in an include
    // guard whose macro is (still) defined. A guard-less header (glibc's
    // bits/mathcalls.h, multi-included with a different _Mdouble_ per
    // pass) is re-tokenized every time, exactly like gcc.
    auto gi = include_guard_by_file.find(canonical);
    if ( gi == include_guard_by_file.end() )
    {
	include_guard_by_file[canonical] = detect_include_guard(canonical);
	return true;
    }
    const std::string &guard = gi->second;
    if ( guard.empty() )
	return true;
    pack_record_branch_macro(guard);	// B4a: guard definedness gates inclusion
    return define_map.find(guard) == define_map.end()
	&& macro_map.find(guard) == macro_map.end();
}

static bool file_exists(const std::string &path)
{
    std::ifstream probe(path.c_str(), std::ios::binary);
    return probe.good();
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
	if ( file_exists(candidates[i]) )
	{
	    outpath = candidates[i];
	    return true;
	}
    }
    return false;
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
    // pervasive ignored specifiers `inline` (erased), `noexcept` and `alignas`
    // (special lexer handling) keep their existing treatment. The erased
    // specifiers `constexpr`/`consteval`/`constinit` are registered below AFTER
    // being removed from the erase map, and need decl-specifier consume handling.
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
    static TokenDataType tkWCHAR_T("wchar_t", ddINT32);
    static TokenDataType tkCHAR8_T("char8_t", ddUINT8);
    static TokenDataType tkCHAR16_T("char16_t", ddUINT16);
    static TokenDataType tkCHAR32_T("char32_t", ddUINT32);
    static TokenDataType tkMAX_ALIGN_T("max_align_t", ddMAX_ALIGN_T);
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
    datatype_map[tkINT64.str] = &tkINT64;
    datatype_map[tkUINT8.str] = &tkUINT8;
    datatype_map[tkUINT16.str] = &tkUINT16;
    datatype_map[tkUINT24.str] = &tkUINT24;
    datatype_map[tkUINT32.str] = &tkUINT32;
    datatype_map[tkUINT64.str] = &tkUINT64;
    datatype_map[tkFLOAT.str] = &tkFLOAT;
    datatype_map[tkDOUBLE.str] = &tkDOUBLE;
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
    datatype_map[tkFLOAT32.str] = &tkFLOAT32;
    datatype_map[tkFLOAT64.str] = &tkFLOAT64;
    datatype_map[tkFLOAT128.str] = &tkFLOAT128;
    datatype_map[tkFLOAT32X.str] = &tkFLOAT32X;
    datatype_map[tkFLOAT64X.str] = &tkFLOAT64X;
    datatype_map[tkDECIMAL32.str] = &tkDECIMAL32;
    datatype_map[tkDECIMAL64.str] = &tkDECIMAL64;
    datatype_map[tkDECIMAL128.str] = &tkDECIMAL128;
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

TokenBase *Program::make_real(double value)
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
		    size_t slash = path.rfind('/');
		    std::string base = (slash != std::string::npos) ? path.substr(slash + 1) : path;
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
		    // angle-bracket includes: prefer real precompiled headers, then
		    // text-embedded compatibility headers, then filesystem source.
		    // #include_next is POSITIONAL (continue the path search after
		    // the current header's dir) — it must never resolve through
		    // the named PCH/embedded caches, only the filesystem walk.
		    if ( is_system && !is_include_next )
		    {
			if ( !suppress_auto_include_scan
			  && pending_auto_include_headers.count(incfile) )
			{
			    DBG(std::cout << "#include <" << incfile
				<< "> deferred to auto-include prelude" << std::endl);
			    return getToken();
			}
			// Phase 6 (--forest-bind): a grove-backed system header
			// BINDS instead of tokenizing — install its PP-export
			// delta along the include DAG (forest_bind_include), then
			// restore the forest's typed decl records into the symbol
			// tables (forest_restore_decls, once per compile), and
			// return WITHOUT re-parsing the header. Non-forest headers
			// fall through to live parse. Gated on forest_bind_enabled
			// so the default path is one predicted branch.
			if ( forest_bind_enabled )
			{
			    int fu = forest_unit_for_include(incfile);
			    if ( fu >= 0 )
			    {
				DBG(std::cout << "#include <" << incfile
				    << "> bound to grove unit " << fu << " ("
				    << bind_forest->unit_name(fu) << ")" << std::endl);
				forest_bind_include((uint32_t)fu);
				if ( !forest_decls_restored )
				{
				    forest_restore_decls(*bind_forest);
				    forest_decls_restored = true;
				}
				return getToken();
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
			bool name_already_included = include_already_seen(include_key);
			bool resolves_named = false;
			std::string pch_path;
			if ( find_filesystem_precompiled_header(*this, incfile, true, pch_path) )
			    resolves_named = true;
			else if ( find_precompiled_header(incfile) )
			    resolves_named = true;
			else if ( find_embedded_header(incfile)
			       && is_embedded_header_allowed(incfile) )
			    resolves_named = true;
			if ( resolves_named && name_already_included )
			{
			    // B4a: the include EDGE exists in the source even when
			    // the once-only dedup skips re-tokenization — the bind-
			    // time PP-export composition walks these edges.
			    pack_record_edge(pch_path.empty() ? incfile : pch_path);
			    DBG(std::cout << "#include <" << incfile << "> skipped (already included)" << std::endl);
			    return getToken();
			}
			if ( resolves_named )
			    should_tokenize_include(include_key);   // record the name
			if ( !pch_path.empty() )
			{
			    DBG(std::cout << "#include <" << incfile << "> (precompiled file "
				<< pch_path << ")" << std::endl);
			    std::deque<TokenBase *> pch_tokens;
			    if ( load_precompiled_header_file(pch_path, pch_tokens) )
			    {
				push_precompiled_header_tokens(*this, pch_path, pch_tokens);
				return getToken();
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
				return getToken();
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
			if ( embedded )
			{
			    DBG(std::cout << "#include <" << incfile << "> (embedded)" << std::endl);
			    pack_record_edge(incfile);	// B4a: includer -> includee, pre-swap
			    Source saved = std::move(source);
			    bool saved_suppress_auto_include_scan = suppress_auto_include_scan;
			    suppress_auto_include_scan = true;
			    source = Source();
			    source.fname(incfile.c_str());
			    { ReadTimer _rt(_read_seconds); source.str(*embedded); }
			    _input_bytes += embedded->size();	// --show-stats: embedded header bytes
			    TokenBase *itb;
			    const char *_interned1 = intern_file(incfile);
			    pack_note_unit(_interned1);
			    while ( (itb = getRealToken()) )
			    {
				itb->file = _interned1;
				push_token_with_literal_concat(itb);
			    }
			    source = std::move(saved);
			    suppress_auto_include_scan = saved_suppress_auto_include_scan;
			    // flag headers for deferred registration during parse init
			    mark_embedded_include_flag(incfile);
			    return getToken();
			}
		    }
		    std::string full_path = is_include_next
			? resolve_include_next_path(incfile)
			: resolve_include_path(incfile, is_system);
		    if ( !should_tokenize_include(full_path) )
		    {
			pack_record_edge(full_path);	// B4a: edge survives the dedup skip
			DBG(std::cout << "#include "
			    << (is_system ? "<" : "\"") << full_path
			    << (is_system ? ">" : "\"")
			    << " skipped (already included)" << std::endl);
			return getToken();
		    }
		    DBG(std::cout << "#include "
			<< (is_system ? "<" : "\"") << full_path
			<< (is_system ? ">" : "\"") << std::endl);
		    pack_record_edge(full_path);	// B4a: includer -> includee, pre-swap
		    // save current source, tokenize included file
		    Source saved = std::move(source);
		    bool saved_suppress_auto_include_scan = suppress_auto_include_scan;
		    suppress_auto_include_scan = true;
		    source = Source();
		    std::ifstream incf(full_path.c_str());
		    if ( !incf )
		    {
			suppress_auto_include_scan = saved_suppress_auto_include_scan;
			source = std::move(saved); // restore before throwing
			Throw << "Failed to open include file: " << full_path.c_str() << flush;
		    }
		    source.fname(full_path.c_str());
		    {
			ReadTimer _rt(_read_seconds);
			incf.seekg(0, std::ios::end);	// --show-stats: filesystem header bytes
			if ( incf.tellg() > 0 ) _input_bytes += (size_t)incf.tellg();
			incf.seekg(0);
			source.copybuf(incf.rdbuf());
		    }
		    TokenBase *itb;
		    const char *_interned2 = intern_file(full_path);
		    pack_note_unit(_interned2);
		    while ( (itb = getRealToken()) )
		    {
			itb->file = _interned2;
			push_token_with_literal_concat(itb);
		    }
		    source = std::move(saved);
		    suppress_auto_include_scan = saved_suppress_auto_include_scan;
		    return getToken(); // continue with current file
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
			handle = dlopen(libname.c_str(), RTLD_LAZY | RTLD_GLOBAL);
			if ( !handle )
			{
			    std::string err = "Failed to load library: " + libname + ": " + dlerror();
			    Throw << err.c_str() << flush;
			}
			DBG(std::cout << "#load \"" << libname << "\" as " << ns_name << std::endl);
			loaded_lib_paths.push_back(libname);
		    }
		    else
		    {
			handle = dlopen(NULL, RTLD_LAZY | RTLD_GLOBAL);
			DBG(std::cout << "#load \"" << libname << "\" as " << ns_name
				      << " — auto-load off, bound to global scope" << std::endl);
		    }
		    dlopen_map[ns_name] = handle;
		    namespace_map[ns_name]; // create empty namespace
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
		    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			source.get();
		    return getToken();
		}
		if ( directive == "ifdef" || directive == "ifndef" )
		{
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string name;
		    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
			name += source.get();
		    bool defined = define_map.count(name) > 0 || macro_map.count(name) > 0;
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
			    if ( arg == "push" )
			    {
				while ( source.peek() == ' ' || source.peek() == '\t' || source.peek() == ',' )
				    source.get();
				int val = 0;
				while ( source.good() && isdigit(source.peek()) )
				    val = val * 10 + (source.get() - '0');
				_pack_stack.push(val ? val : 1);
				DBG(std::cout << "#pragma pack(push, " << (val ? val : 1) << ")" << std::endl);
			    }
			    else if ( arg == "pop" )
			    {
				if ( !_pack_stack.empty() )
				    _pack_stack.pop();
				DBG(std::cout << "#pragma pack(pop)" << std::endl);
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
				order.push_back(name);
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
	    return make_str(word);
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
		    if ( long_count >= 1 )
			return has_u_suffix ? (DataDef *)&ddUINT64 : (DataDef *)&ddINT64;
		    if ( has_u_suffix )
			return &ddUINT32;
		    // C integer literal type rules (no suffix):
		    // Hex/octal: int → unsigned int → long → unsigned long
		    // Decimal:   int → long → long long (never unsigned)
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
		    char real_type_suffix = 0;
		    if ( source.good() )
		    {
			int c = source.peek();
			if ( c == 'f' || c == 'F' || c == 'l' || c == 'L' )
			{
			    real_type_suffix = (char)c;
			    lit_text += (char)source.get();
			}
			else if ( c == 'd' || c == 'D' )
			{
			    lit_text += (char)source.get();
			    if ( source.good() && (source.peek() == 'd' || source.peek() == 'D') )
				lit_text += (char)source.get();
			}
		    }
		    if ( eat_imag_suffix() )
		    {
			TokenReal *tr = (TokenReal *)make_real(strtod(lit_text.c_str(), NULL));
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
			TokenReal *tr = (TokenReal *)make_real(strtod(lit_text.c_str(), NULL));
			if ( real_type_suffix == 'f' || real_type_suffix == 'F' )
			    tr->setDataType(&ddFLOAT);
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
			char real_type_suffix = 0;
			if ( source.good() )
			{
			    int c = source.peek();
			    if ( c == 'f' || c == 'F' || c == 'l' || c == 'L' )
			    {
				real_type_suffix = (char)c;
				lit_text += (char)source.get();
			    }
			}
			double num = strtod(lit_text.c_str(), NULL);
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
			return make_real(num);
		    }
		    if ( eat_imag_suffix() )
		    {
			TokenInt *ti = (TokenInt *)make_int((int64_t)(uint64_t)v);
			ti->source_text = lit_text;
			ti->setDataType(get_complex_compat_type(&ddINT64));
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
		// C float literal suffixes (f/F, l/L). f marks a float (4-byte
		// real); madc doesn't currently distinguish float-vs-double
		// literals at lex time (TokenReal is always double-precision),
		// so we just consume the suffix char.
		char real_type_suffix = 0;
		if ( source.good() )
		{
		    int c = source.peek();
		    if ( c == 'f' || c == 'F' || c == 'l' || c == 'L' )
		    {
			real_type_suffix = (char)c;
			lit_text += (char)source.get();
		    }
		    else if ( c == 'd' || c == 'D' )
		    {
			lit_text += (char)source.get();
			if ( source.good() && (source.peek() == 'd' || source.peek() == 'D') )
			    lit_text += (char)source.get();
		    }
		}
		double num = strtod(lit_text.c_str(), NULL);
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
		    if ( real_type_suffix == 'f' || real_type_suffix == 'F' )
			tr->setDataType(&ddFLOAT);
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
		if ( word == "L" && source.good()
		  && (source.peek() == '"' || source.peek() == '\'') )
		    return read_wide_literal();
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
		    int depth = 1;
		    bool at_line_start = false; // track whether next non-ws char is start of line
		    int macro_ifdef_skip = 0; // >0 means we're skipping a false #ifdef/#else branch
		    int macro_ifdef_depth = 0; // tracks #ifdef nesting inside macro args
		    while ( source.good() && depth > 0 )
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
				bool defined = define_map.count(name) > 0 || macro_map.count(name) > 0;
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
			if ( mc == '(' ) { ++depth; arg += mc; }
			else if ( mc == ')' ) { --depth; if (depth > 0) arg += mc; }
			else if ( mc == ',' && depth == 1 )
			{
			    // trim whitespace from arg
			    while ( !arg.empty() && (arg.front() == ' ' || arg.front() == '\t') ) arg.erase(arg.begin());
			    while ( !arg.empty() && (arg.back() == ' ' || arg.back() == '\t') ) arg.pop_back();
			    args.push_back(arg);
			    arg.clear();
			}
			else if ( mc == '"' )
			{
			    arg += mc;
			    while ( source.good() && source.peek() != '"' )
			    {
				if ( source.peek() == '\\' ) arg += source.get();
				arg += source.get();
			    }
			    if ( source.peek() == '"' ) arg += source.get();
			}
			else if ( mc == '\'' )
			{
			    // Char literal — copy verbatim through the closing
			    // `'` so any `(`, `)`, `,`, `"` inside the literal
			    // (e.g. `')'`, `','`, `'"'`) doesn't disturb the
			    // macro arg parser. Honour `\\` escapes.
			    arg += mc;
			    while ( source.good() && source.peek() != '\'' )
			    {
				if ( source.peek() == '\\' ) arg += source.get();
				arg += source.get();
			    }
			    if ( source.peek() == '\'' ) arg += source.get();
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
		    // C standard: pre-expand macros in arguments before
		    // substitution (except for # and ## operands, but we
		    // expand uniformly for simplicity). This handles nested
		    // macro calls like UMIN(x, UMIN(y, z)).
		    for ( size_t i = 0; i < args.size(); ++i )
		    {
			std::string &a = args[i];
			// Quick check: does the argument contain any known macro name?
			// A naive alpha check triggers on hex literals (0x1F) and
			// integer suffixes (LU/ULL), whose round-trip through the
			// tokenizer loses the original representation.  Scan for
			// actual identifier words and see if any match a define.
			bool has_macro = false;
			for ( size_t j = 0; j < a.size() && !has_macro; ++j )
			{
			    if ( !(isalpha((unsigned char)a[j]) || a[j] == '_') )
				continue;
			    std::string id;
			    while ( j < a.size() && (isalnum((unsigned char)a[j]) || a[j] == '_') )
				id += a[j++];
			    if ( define_map.count(id) || macro_map.count(id) )
				has_macro = true;
			}
			if ( !has_macro ) continue;
			// Push arg text through the tokenizer to expand macros
			Source saved = std::move(source);
			source = Source();
			source.str(a);
			std::string expanded_arg;
			TokenBase *at;
			while ( (at = getToken()) )
			{
			    switch ( at->type() )
			    {
				case TokenType::ttSpace: expanded_arg += ' '; break;
				case TokenType::ttTab:   expanded_arg += '\t'; break;
				case TokenType::ttEOL:   expanded_arg += '\n'; break;
				case TokenType::ttOperator:
				case TokenType::ttSymbol:
				    expanded_arg += (char)at->get(); break;
				case TokenType::ttMultiOp:
				    expanded_arg += ((TokenMultiOp *)at)->str; break;
				case TokenType::ttString:
				    expanded_arg += '"';
				    expanded_arg += ((TokenIdent *)at)->spelling();
				    expanded_arg += '"';
				    break;
				case TokenType::ttChar:
				    expanded_arg += '\'';
				    expanded_arg += (char)at->get();
				    expanded_arg += '\'';
				    break;
				default:
				    if ( auto *ti = dynamic_cast<TokenIdent *>(at) )
					expanded_arg += ti->spelling();
				    else if ( at->type() == TokenType::ttInteger )
				    {
					TokenInt *tki = static_cast<TokenInt *>(at);
					if ( !tki->source_text.empty() )
					    expanded_arg += tki->source_text;
					else
					{
					    char buf[32];
					    snprintf(buf, sizeof(buf), "%ld", (long)at->get());
					    expanded_arg += buf;
					}
				    }
				    else
					expanded_arg += (char)at->get();
				    break;
			    }
			}
			source = std::move(saved);
			a = expanded_arg;
		    }
		    std::map<std::string, const std::string *> param_map;
		    std::string named_varargs;
		    std::string empty_macro_arg;   // for params supplied no argument
		    bool has_named_varargs = macro.variadic && !macro.variadic_param.empty();
		    size_t fixed_param_count = macro.params.size();
		    if ( has_named_varargs && fixed_param_count > 0 )
			--fixed_param_count;
		    for ( size_t i = 0; i < macro.params.size() && i < args.size(); ++i )
		    {
			if ( has_named_varargs && i == macro.params.size() - 1 )
			{
			    named_varargs = args[i];
			    for ( size_t j = i + 1; j < args.size(); ++j )
			    {
				named_varargs += ", ";
				named_varargs += args[j];
			    }
			    param_map[macro.params[i]] = &named_varargs;
			}
			else
			    param_map[macro.params[i]] = &args[i];
		    }
		    // A macro called with fewer arguments than parameters binds the
		    // missing params to EMPTY (C semantics) — e.g. `STR()` for
		    // `#define STR(x) #x` binds x to "" so `#x` stringizes to "",
		    // not a stray literal `#`. (glibc: __STRING(__USER_LABEL_PREFIX__)
		    // with the prefix empty reaches the inner # with no argument.)
		    for ( size_t i = args.size(); i < macro.params.size(); ++i )
			param_map[macro.params[i]] = &empty_macro_arg;
		    auto stringify_macro_arg = [](const std::string &raw) -> std::string {
			std::string out("\"");
			bool pending_space = false;
			bool wrote = false;
			for ( char c : raw )
			{
			    if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' )
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
			    if ( c == '"' || c == '\\' )
				out += '\\';
			    out += c;
			    wrote = true;
			}
			out += '"';
			return out;
		    };
		    std::string expanded;
		    expanded.reserve(macro.body.size());
		    for ( size_t p = 0; p < macro.body.size(); )
		    {
			char bc = macro.body[p];
			if ( bc == '#' && p + 1 < macro.body.size() && macro.body[p+1] == '#' )
			{
			    expanded += "##";
			    p += 2;
			}
			else if ( bc == '#' )
			{
			    size_t q = p + 1;
			    while ( q < macro.body.size()
				 && (macro.body[q] == ' ' || macro.body[q] == '\t') )
				++q;
			    if ( q < macro.body.size()
			      && (macro.body[q] == '_' || isalpha((unsigned char)macro.body[q])) )
			    {
				size_t start = q;
				while ( q < macro.body.size()
				     && (macro.body[q] == '_' || isalnum((unsigned char)macro.body[q])) )
				    ++q;
				std::string ident = macro.body.substr(start, q - start);
				auto it = param_map.find(ident);
				if ( it != param_map.end() )
				{
				    expanded += stringify_macro_arg(*it->second);
				    p = q;
				    continue;
				}
			    }
			    expanded += bc;
			    ++p;
			}
			else if ( bc == '_' || isalpha((unsigned char)bc) )
			{
			    size_t start = p;
			    while ( p < macro.body.size()
				 && (macro.body[p] == '_' || isalnum((unsigned char)macro.body[p])) )
				++p;
			    std::string ident = macro.body.substr(start, p - start);
			    auto it = param_map.find(ident);
			    if ( it != param_map.end()
			      && !is_prefixed_literal_token(ident, macro.body, p) )
				expanded += *it->second;
			    else
				expanded += ident;
			}
			else
			{
			    expanded += bc;
			    ++p;
			}
		    }
		    // C token-pasting: `A##B` after parameter substitution
		    // fuses the two identifiers into one. Strip every `##`
		    // (and optional whitespace around it) so the lexer sees
		    // a single identifier when it re-tokenizes the
		    // expansion. Required for IMC's COL(x) → C_##x pattern.
		    {
			std::string fused;
			fused.reserve(expanded.size());
			for ( size_t p = 0; p < expanded.size(); )
			{
			    if ( p + 1 < expanded.size() && expanded[p] == '#' && expanded[p+1] == '#' )
			    {
				// Drop trailing whitespace already in fused.
				while ( !fused.empty() && (fused.back() == ' ' || fused.back() == '\t') )
				    fused.pop_back();
				p += 2;
				// Drop leading whitespace after the ##.
				while ( p < expanded.size() && (expanded[p] == ' ' || expanded[p] == '\t') )
				    ++p;
				continue;
			    }
			    fused += expanded[p++];
			}
			expanded.swap(fused);
		    }
		    if ( macro.variadic )
		    {
			std::string varargs;
			for ( size_t i = fixed_param_count; i < args.size(); ++i )
			{
			    if ( !varargs.empty() )
				varargs += ", ";
			    varargs += args[i];
			}
			size_t pos = 0;
			while ( (pos = expanded.find("__VA_ARGS__", pos)) != std::string::npos )
			{
			    expanded.replace(pos, strlen("__VA_ARGS__"), varargs);
			    pos += varargs.size();
			}
		    }
		    DBG(std::cout << "macro expand " << word << " -> " << expanded << std::endl);
		    source.pushback_macro(expanded, word);
		    return getToken();
		}
			// #define substitution: inject the define value into the source stream
			if ( define_map.count(sid) && !source.macro_disabled(word) )
			{
			    if ( word == "inline"
			      && next_source_word_is(source, "namespace") )
				return make_ident(word);
			    std::string &val = define_map[sid];
			    if ( !val.empty() )
			    {
			// Builtin libc aliases such as __builtin_strcmp -> strcmp
			// should resolve to the target identifier directly instead
			// of re-entering the macro rescanner. Otherwise user macros
			// like `#define strcmp __builtin_strcmp` recurse forever.
			if ( word.compare(0, 10, "__builtin_") == 0
			  && is_identifier_spelling(val)
			  && !builtin_alias_needs_retokenize(word, val) )
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
		    source.pushback(quoted);
		    return getToken();
		}
		if ( word == "__LINE__" )
		{
		    source.pushback(std::to_string(source.line()));
		    return getToken();
		}
		// __FUNCTION__ / __func__ / __PRETTY_FUNCTION__: keep these
		// as magic identifiers. madc tokenizes the whole file before
		// parsing, so parseExpression resolves them after cur_func_name
		// is known.
		if ( word == "__FUNCTION__" || word == "__func__"
		  || word == "__PRETTY_FUNCTION__" )
		    return make_ident(word);
		// noexcept / noexcept(expr): madc ignores exception
		// specifications. Strip the optional (...) by BALANCED parens
		// — NOT via a function-like macro, whose comma-splitting breaks
		// on a template-id condition such as
		// noexcept(is_nothrow_constructible<T, Args...>::value) (the
		// preprocessor does not treat <...> as grouping).
		if ( word == "noexcept" )
		{
		    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t' || source.peek() == '\n' || source.peek() == '\r') )
			source.get();
		    if ( source.peek() == '(' )
		    {
			int depth = 0;
			do {
			    char c = source.get();
			    if ( c == '(' ) ++depth;
			    else if ( c == ')' ) --depth;
			} while ( source.good() && depth > 0 );
		    }
		    return getToken();
		}
		// Most GCC attributes are no-ops for madc. Preserve the few
		// layout/type-shaping ones the parser understands and skip
		// the rest.
		if ( word == "__attribute__" || word == "__attribute" )
		{
		    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t' || source.peek() == '\n' || source.peek() == '\r') )
			source.get();
		    std::string attr_text;
		    if ( source.peek() == '(' )
		    {
			int depth = 0;
			do {
			    char c = source.get();
			    attr_text += c;
			    if ( c == '(' ) ++depth;
			    else if ( c == ')' ) --depth;
			} while ( source.good() && depth > 0 );
		    }
		    if ( gnu_attribute_text_has_name(attr_text, "packed")
		      || gnu_attribute_text_has_name(attr_text, "aligned")
		      || gnu_attribute_text_has_name(attr_text, "mode")
		      || gnu_attribute_text_has_name(attr_text, "scalar_storage_order")
		      || gnu_attribute_text_has_name(attr_text, "vector_size")
		      || gnu_attribute_text_has_name(attr_text, "alias")
		      || gnu_attribute_text_has_name(attr_text, "no_instrument_function")
		      || gnu_attribute_text_has_name(attr_text, "optimize") )
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
		  || word == "__complex" )
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
		    };
		    int counter = compound_type_specifier_flag(word);
		    // Accumulate subsequent type-specifier keywords
		    auto read_word = [&]() -> std::string {
			while ( source.good()
			     && (source.peek() == ' ' || source.peek() == '\t'
			      || source.peek() == '\n' || source.peek() == '\r') )
			    source.get();
			std::string w;
			while ( source.good() && (isalnum(source.peek()) || source.peek() == '_') )
			    w += source.get();
			return w;
		    };
		    // Read ahead, accumulating type specifier keywords
		    std::vector<std::string> consumed;
		    while ( true )
		    {
			std::string w = read_word();
			int flag = compound_type_specifier_flag(w);
			if ( flag )
			{
			    counter += flag;
			    consumed.push_back(w);
			}
			else if ( define_map.find(w) != define_map.end() )
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
			    // Not a type specifier — push it back
			    if ( !w.empty() )
				source.pushback_reread(std::string(" ") + w);
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
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT64); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("long", ddINT64);
			case TS_UNSIGNED + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT64); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned long", ddUINT64);
			case TS_LONG + TS_LONG:
			case TS_LONG + TS_LONG + TS_INT:
			case TS_SIGNED + TS_LONG + TS_LONG:
			case TS_SIGNED + TS_LONG + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT64); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("long long", ddINT64);
			case TS_UNSIGNED + TS_LONG + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT64); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("unsigned long long", ddUINT64);
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
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddDOUBLE); return make_datatype(dd->name.c_str(), *dd); }
			    return make_datatype("long double", ddDOUBLE);
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

// evaluate #if condition: supports defined(NAME), !, &&, ||, identifiers,
// and integer constants. This is enough for the header guards and platform
// feature checks used by the imported SMAUG sources.
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
	size_t i = 0;
	bool preserve_defined_operand = false;
	while ( i < expr.size() )
	{
	    // Copy non-identifier characters
	    if ( !isalpha((unsigned char)expr[i]) && expr[i] != '_' )
	    {
		out += expr[i++];
		continue;
	    }
	    // Extract identifier
	    std::string word;
	    while ( i < expr.size() && (isalnum((unsigned char)expr[i]) || expr[i] == '_') )
		word += expr[i++];
	    // B4a: every identifier a #if/#elif condition consults (including
	    // `defined` operands, which the evaluator resolves later) is a
	    // branch-relevant macro name for the pack container.
	    if ( word != "defined" )
		pack_record_branch_macro(word);
	    // Don't expand 'defined' — it's a #if operator
	    if ( word == "defined" )
	    {
		out += word;
		preserve_defined_operand = true;
		continue;
	    }
	    if ( preserve_defined_operand )
	    {
		out += word;
		preserve_defined_operand = false;
		continue;
	    }
	    // Look up in define_map
	    auto it = define_map.find(word);
	    if ( it != define_map.end() )
	    {
		out += it->empty() ? "1" : *it;
		changed = true;
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
		size_t j = i;
		while ( j < expr.size() && (expr[j] == ' ' || expr[j] == '\t') )
		    ++j;
		if ( j < expr.size() && expr[j] == '(' )
		{
		    const MacroDef &m = macro_map[word];
		    std::vector<std::string> margs;
		    std::string marg;
		    int mdepth = 0;
		    ++j; // consume '('
		    for ( ; j < expr.size(); ++j )
		    {
			char mc = expr[j];
			if ( mc == '(' ) { ++mdepth; marg += mc; }
			else if ( mc == ')' )
			{
			    if ( mdepth == 0 ) { ++j; break; }
			    --mdepth; marg += mc;
			}
			else if ( mc == ',' && mdepth == 0 )
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
		    // Substitute params in a single pass over the body so an
		    // argument matching a later parameter name isn't cascaded.
		    const std::string &body = m.body;
		    std::string expanded;
		    size_t b = 0;
		    while ( b < body.size() )
		    {
			if ( !isalpha((unsigned char)body[b]) && body[b] != '_' )
			{ expanded += body[b++]; continue; }
			std::string bw;
			while ( b < body.size()
			     && (isalnum((unsigned char)body[b]) || body[b] == '_') )
			    bw += body[b++];
			bool subst = false;
			for ( size_t pi2 = 0; pi2 < m.params.size(); ++pi2 )
			    if ( bw == m.params[pi2] )
			    {
				expanded += pi2 < margs.size() ? margs[pi2] : "";
				subst = true;
				break;
			    }
			if ( !subst && m.variadic
			  && (bw == "__VA_ARGS__"
			      || (!m.variadic_param.empty() && bw == m.variadic_param)) )
			{
			    for ( size_t va = m.params.size(); va < margs.size(); ++va )
			    {
				if ( va > m.params.size() ) expanded += ", ";
				expanded += margs[va];
			    }
			    subst = true;
			}
			if ( !subst )
			    expanded += bw;
		    }
		    out += "(" + expanded + ")";
		    i = j;
		    changed = true;
		}
		else
		{
		    // Function-like macro name with NO argument list: keep the
		    // historical behavior (treated as 1 in #if context).
		    out += "1";
		    changed = true;
		}
	    }
	    else
		out += word; // leave as-is (will become 0 in the evaluator)
	}
	expr = out;
	if ( !changed ) break;
    }
    return expr;
}

bool Program::evaluateIfCondition()
{
    std::string raw_expr;
    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
	raw_expr += source.get();

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
	    int64_t value = parse_or();
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
		return (define_map.count(name) > 0 || macro_map.count(name) > 0) ? 1 : 0;
	    }

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

    return parse_or() != 0;
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

	while ( (tb=getToken()) )
	{
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

// Best-effort source spelling of a lex-time token. NOTE: numeric literals store
// the parsed value, not the original text (0x1F -> "31"), and string escapes are
// not preserved — true byte-faithful reconstruction needs tokens to retain raw
// source text (a follow-on). Sufficient to demonstrate trivia retention on plain
// source. Keywords/identifiers/types/comments are all TokenIdent-derived.
static std::string token_spelling(TokenBase *tb)
{
    switch ( tb->type() )
    {
	case TokenType::ttString:
	    if ( TokenIdent *ti = dynamic_cast<TokenIdent *>(tb) )
		return std::string("\"") + ti->spelling() + "\"";
	    return std::string();
	case TokenType::ttVariable:
	    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) ) return tv->var.name;
	    return std::string();
	case TokenType::ttInteger: return std::to_string(tb->ival());
	case TokenType::ttReal:    return std::to_string(tb->dval());
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

// Reconstruct source text from the token stream (full-fidelity mode): each
// token's leading trivia followed by its spelling. Requires keep_trivia to have
// been set before tokenizing.
std::string Program::reconstruct_source()
{
    std::string out;
    for ( TokenBase *tb : tokens )
    {
	out += tb->leading_trivia;
	out += token_spelling(tb);
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
	char *env_columns = getenv("COLUMNS");
	size_t term_columns;
	std::string ln;

	if ( env_columns )
	    term_columns = atoi(env_columns);
	else
	    term_columns = 80;

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

	if ( ln.length()+5 > term_columns )
	{
	    ln = "  ..." + ln.substr(col);
	    std::cerr << ln << std::endl;
	    std::cerr << std::setw(4) << ' ' << "\e[1;32m^\e[m" << std::endl;
	    return;
	}
	std::cerr << ln << std::endl;
	if ( col > 1 )
	    std::cerr << std::setw(col-1) << ' ';
	std::cerr << "\e[1;32m^\e[m" << std::endl;
}

int throwbuf::sync()
{
    cerr << endl;
    if ( _tb )
    {
	cerr << ANSI_WHITE << (_src ? _src->fname() : "???") << ':' << _tb->line << ':' << _tb->column 
	     << ": \e[1;31merror:\e[1;37m " << str() << ANSI_RESET << endl;
	if ( _src )
	    _src->showerror(_tb->line, _tb->column);
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
	set_error(Program::DiagnosticPhase::lexer, "Failed to open file", fname, 0, 0);
	print_last_diagnostic(error());
	return NULL;
    }

    _tokenizer_init();

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
	while ( (tb=getRealToken()) )
	{
	    tb->file = fname;
//	    tb->line = source.line();
//	    tb->column = source.column();
	    push_token_with_literal_concat(tb);
        }
    }
    catch(const char *err_msg)
    {
	set_error(Program::DiagnosticPhase::lexer, err_msg ? err_msg : "(null error message)", fname, source.line(), source.column());
	print_last_diagnostic(error());
	return NULL;
    }
    catch(TokenIdent *ti)
    {
	set_error(Program::DiagnosticPhase::lexer, std::string("use of undeclared identifier '") + ti->spelling() + '\'', fname, source.line(), source.column());
	print_last_diagnostic(error());
	return NULL;
    }
    catch(TokenBase *tb)
    {
	set_error(Program::DiagnosticPhase::lexer, std::string("unexpected token type ") + std::to_string((int)tb->type()), fname, source.line(), source.column());
	print_last_diagnostic(error());
	return NULL;
    }
    catch(std::exception &e)
    {
	if ( !last_error.has_error )
	    set_error(Program::DiagnosticPhase::lexer, Throw.str().empty() ? e.what() : Throw.str(), fname, source.line(), source.column());
	return NULL;
    }

    DBG(std::cout << "Program::tokenize() finished tokenizing" << std::endl);

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
	while ( (tb=getRealToken()) )
	{
	    tb->file = fname;
	    push_token_with_literal_concat(tb);
	}
    }
    catch(const char *err_msg)
    {
	set_error(Program::DiagnosticPhase::lexer, err_msg ? err_msg : "(null error message)",
		  fname, source.line(), source.column());
	print_last_diagnostic(error());
	return NULL;
    }
    catch(TokenIdent *ti)
    {
	set_error(Program::DiagnosticPhase::lexer,
		  std::string("use of undeclared identifier '") + ti->spelling() + '\'',
		  fname, source.line(), source.column());
	print_last_diagnostic(error());
	return NULL;
    }
    catch(TokenBase *tb)
    {
	set_error(Program::DiagnosticPhase::lexer,
		  std::string("unexpected token type ") + std::to_string((int)tb->type()),
		  fname, source.line(), source.column());
	print_last_diagnostic(error());
	return NULL;
    }
    catch(std::exception &e)
    {
	if ( !last_error.has_error )
	    set_error(Program::DiagnosticPhase::lexer,
		      Throw.str().empty() ? e.what() : Throw.str(),
		      fname, source.line(), source.column());
	return NULL;
    }

    DBG(std::cout << "Program::tokenize_buffer() finished tokenizing" << std::endl);

    tkProgram = new TokenProgram();
    tkFunction = tkProgram;
    flush_forest_pending_globals();	// v13: globals staged during #include bind

    tkProgram->source = effective_name;
    tkProgram->is = new std::stringstream(source_text);
    tkProgram->lines = source.line()-1;
    tkProgram->bytes = source_text.size();

    return tkProgram;
}
