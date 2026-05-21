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
#include <queue>
#include <stack>
#include <functional>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;
using namespace asmjit;

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

static int64_t read_binary_literal(Source &source)
{
    int64_t bv = 0;
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
    // `typedef unsigned long size_t;` and similar declaration heads are
    // defining the identifier, not using the standard header surface.
    // Auto-including here injects the embedded header in the middle of
    // the declarator and leaves a duplicate alias token behind.
    for ( std::deque<TokenBase *>::reverse_iterator it = tokens.rbegin();
	  it != tokens.rend(); ++it )
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

    std::string include_key = std::string("<") + header + ">";
    if ( !should_tokenize_include(include_key) )
	return false;

    const std::string *embedded = find_embedded_header(header);
    if ( !embedded )
	return false;
    if ( !is_embedded_header_allowed(header) )
	Throw << "embedded header '" << header
	      << "' is not allowed by registration policy" << flush;

    source.pushback(word);

    Source saved = std::move(source);
    source = Source();
    source.fname(header);
    source.str(*embedded);
    TokenBase *itb;
    const char *interned = intern_file(header);
    while ( (itb = getRealToken()) )
    {
	itb->file = interned;
	push_token_with_string_concat(itb);
    }
    source = std::move(saved);
    mark_embedded_include_flag(header);
    return true;
}

// Walk back through recently emitted tokens to decide whether the
// current identifier is at a declaration / definition head, i.e.
// `<type> [*...] <ident> (...)`. When so, we must suppress
// function-like macro expansion: otherwise `#define bug(...) ((void)0)`
// above a later `void bug(const char *, ...)` definition eats the
// declarator and the parse fails. Skips pointer decorators; stops at
// the first non-`*` token and classifies it as type / qualifier /
// typedef-identifier (→ decl head) or anything else (→ not decl head).
static bool looks_like_decl_head(const std::deque<TokenBase *> &tokens)
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
TokenNAMESPACE	tkNAMESPACE;
TokenPREFER	tkPREFER;
TokenDEFER	tkDEFER;
TokenVECTOR	tkVECTOR;
TokenMAP	tkMAP;
TokenSET	tkSET;
TokenLIST	tkLIST;

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
TokenSTRING	tkSTRING;
TokenSSTREAM	tkSSTREAM;
TokenARRAY	tkARRAY;
TokenIFSTREAM	tkIFSTREAM;
TokenOFSTREAM	tkOFSTREAM;
TokenFSTREAM	tkFSTREAM;
TokenLPSTR	tkLPSTR;
TokenAUTO	tkAUTO;


void Program::push_token_with_string_concat(TokenBase *tb)
{
    if ( tb->type() == TokenType::ttString
      && !tokens.empty()
      && tokens.back()->type() == TokenType::ttString )
    {
	((TokenStr *)tokens.back())->str += ((TokenStr *)tb)->str;
	delete tb;
	return;
    }
    tokens.push_back(tb);
}

void Program::_tokenizer_init()
{

    tkProgram = NULL;
    tkFunction = NULL;
    _cur_token = NULL;
    _prv_token = NULL;
    _include_iostream = false;
    _include_stdio = false;
    _include_string = false;
    included_files.clear();
    add_keywords();
    add_datatypes();
    struct_map["teststruct"] = &ddTESTSTRUCT;

    // Ignored C storage hints. Type qualifiers are real tokens so
    // macro token-pasting can still see their spelling.
    define_map["inline"] = "";
    define_map["__inline__"] = "";
    define_map["__inline"] = "";
    define_map["__extension__"] = "";
    // _Alignas(N) is a C11 keyword — consume like __attribute__
    // The lexer handles it by stripping the specifier and its parens.
    define_map["_Alignas"] = "__attribute__";
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
    define_map["__FLT_EPSILON__"] = "1.19209290e-7F";
    define_map["__DBL_EPSILON__"] = "2.2204460492503131e-16";
    define_map["__BIGGEST_ALIGNMENT__"] = "16";

    // GCC predefined macros for C compatibility
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
    // __builtin_va_start/end — need to be function-like macros
    {
	MacroDef m;
	m.params = {"__ap", "__last"};
	m.body = "__ap = __va_args";
	macro_map["__builtin_va_start"] = m;
    }
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
    define_map["__x86_64__"] = "1";
    define_map["__LP64__"] = "1";
    define_map["__BYTE_ORDER__"] = "1234";
    define_map["__ORDER_LITTLE_ENDIAN__"] = "1234";

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

    // __builtin_*_overflow: overflow-checking arithmetic (GCC extension).
    // Mapped to helper functions in va_helpers.cpp that use __int128.
    define_map["__builtin_add_overflow"] = "__madc_add_overflow";
    define_map["__builtin_sub_overflow"] = "__madc_sub_overflow";
    define_map["__builtin_mul_overflow"] = "__madc_mul_overflow";

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
    // __builtin_constant_p(expr) — always return 0 (not a constant)
    {
	MacroDef m;
	m.params = {"__expr"};
	m.body = "0";
	macro_map["__builtin_constant_p"] = m;
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
    // __builtin_signbit(x) → (x < 0.0) — simplified
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "((__x) < 0.0 ? 1 : 0)";
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
    // __builtin_classify_type(x) → 0 (integer type, simplified)
    {
	MacroDef m;
	m.params = {"__x"};
	m.body = "0";
	macro_map["__builtin_classify_type"] = m;
    }
    define_map["__builtin_alloca"] = "malloc"; // alloca = stack alloc, map to malloc for now
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
	// TODO: these paths should come from ./configure
	static const char *sys_paths[] = {
	    "/usr/local/include/",
	    "/usr/include/",
	    "/usr/include/x86_64-linux-gnu/",
	    NULL
	};
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
    if ( include_already_seen(canonical) )
	return false;
    included_files[canonical] = true;
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
    keyword_map[tkTRY.str] = &tkTRY;
    keyword_map[tkCATCH.str] = &tkCATCH;
    keyword_map[tkTHROW.str] = &tkTHROW;
    keyword_map[tkSWITCH.str] = &tkSWITCH;
    keyword_map[tkWHILE.str] = &tkWHILE;
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
    keyword_map[tkUSING.str] = &tkUSING;
    keyword_map[tkNAMESPACE.str] = &tkNAMESPACE;
    keyword_map[tkPREFER.str] = &tkPREFER;
    keyword_map[tkDEFER.str] = &tkDEFER;
    keyword_map[tkVECTOR.str] = &tkVECTOR;
    keyword_map[tkMAP.str] = &tkMAP;
    keyword_map[tkSET.str] = &tkSET;
    // `list` intentionally omitted from keyword_map so it doesn't shadow
    // the C identifier `list`. Use `std::list<T>` instead.
    // keyword_map[tkLIST.str] = &tkLIST;
}

// add static tokens for base data types
void Program::add_datatypes()
{
    static TokenDataType tkPTRDIFF("ptrdiff_t", ddINT64);
    static TokenDataType tkSIZE_T("size_t", ddUINT64);
    static TokenDataType tkWCHAR_T("wchar_t", ddINT32);

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
    datatype_map[tkWCHAR_T.str] = &tkWCHAR_T;
}


// lex and return the next token from the data stream
// TODO: replace top switch with direct dispatch
//       also likely better to replace istream stuff
//       with a character buffer for maximum speed
TokenBase *Program::_getToken()
{
    keyword_map_iter kmi;
    datatype_map_iter bmi;
    string word;
    int ch, cnt, row, col;

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
	case ' ':
	    cnt = 1;
	    while ( source.peek() == ' ' )
	    {
		++cnt;
		source.get();
		if ( !source.good() || source.eof() )
		    break;
	    }
	    return new TokenSpace(cnt);
	case '\t':
	    cnt = 1;
	    while ( source.peek() == '\t' )
	    {
		++cnt;
		source.get();
		if ( !source.good() || source.eof() )
		    break;
	    }
	    return new TokenTab(cnt);
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
	    return new TokenEOL(cnt);
	case '=':
	    if (source.peek() == '=')
	    {
		source.get();
		if (source.peek() == '=') { source.get(); return new Token3Eq; } // ===
		return new TokenEquals;					// ==
	    }
	    if (source.peek() == '>') { source.get(); return new TokenFatArrow; } // =>
	    return new TokenAssign;					// =
	case '+':
	    if (source.peek() == '+') { source.get(); return new TokenInc;   }   // ++
	    if (source.peek() == '=') { source.get(); return new TokenAddEq; }   // +=
	    return new TokenAdd;					// +
	case '-':
	    if (source.peek() == '-') { source.get(); return new TokenDec;   }   // --
	    if (source.peek() == '=') { source.get(); return new TokenSubEq; }   // -=
	    if (source.peek() == '>') { source.get(); return new TokenDeRef; }   // ->
	    return new TokenNeg;					// -
	case '*': if (source.peek() != '=') return new TokenMul;		// *
	     source.get(); return new TokenMulEq;				// *=
	case '/':
	    if (source.peek() == '=') { source.get(); return new TokenDivEq; }   // /=
	    if (source.peek() == '/')					// //
	    {
		source.get();
		word = "//";
		while ( source.good() && !source.eof() && source.peek() != '\r' && source.peek() != '\n' )
		    word += source.get();
		return new TokenREM(word);
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
		return new TokenREM(word);
	    }
	    return new TokenDiv;
	case '\\': return new TokenBslsh;
	case '#': // #! is a special comment style for shell script execution
	    if ( source.peek() == '!' )
	    {
		source.get();
		word = "#!";
		while ( source.good() && !source.eof() && source.peek() != '\r' && source.peek() != '\n' )
		    word += source.get();
		return new TokenREM(word);
	    }
	    while ( source.peek() == ' ' || source.peek() == '\t' )
		source.get();
	    // #include directive
	    if ( isalpha(source.peek()) )
	    {
		std::string directive;
		while ( source.good() && !source.eof() && isalpha(source.peek()) )
		    directive += source.get();
		if ( directive == "include" )
		{
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
		    // angle-bracket includes: check embedded headers first
		    if ( is_system )
		    {
			std::string include_key = "<" + incfile + ">";
			if ( !should_tokenize_include(include_key) )
			{
			    DBG(std::cout << "#include <" << incfile << "> skipped (already included)" << std::endl);
			    return getToken();
			}
			const std::string *embedded = find_embedded_header(incfile);
			if ( embedded )
			{
			    if ( !is_embedded_header_allowed(incfile) )
				Throw << "embedded header '" << incfile
				      << "' is not allowed by registration policy" << flush;
			    DBG(std::cout << "#include <" << incfile << "> (embedded)" << std::endl);
			    Source saved = std::move(source);
			    source = Source();
			    source.fname(incfile.c_str());
			    source.str(*embedded);
			    TokenBase *itb;
			    const char *_interned1 = intern_file(incfile);
			    while ( (itb = getRealToken()) )
			    {
				itb->file = _interned1;
				push_token_with_string_concat(itb);
			    }
			    source = std::move(saved);
			    // flag headers for deferred registration during parse init
			    mark_embedded_include_flag(incfile);
			    return getToken();
			}
		    }
		    std::string full_path = resolve_include_path(incfile, is_system);
		    if ( !should_tokenize_include(full_path) )
		    {
			DBG(std::cout << "#include "
			    << (is_system ? "<" : "\"") << full_path
			    << (is_system ? ">" : "\"")
			    << " skipped (already included)" << std::endl);
			return getToken();
		    }
		    DBG(std::cout << "#include "
			<< (is_system ? "<" : "\"") << full_path
			<< (is_system ? ">" : "\"") << std::endl);
		    // save current source, tokenize included file
		    Source saved = std::move(source);
		    source = Source();
		    std::ifstream incf(full_path.c_str());
		    if ( !incf )
		    {
			source = std::move(saved); // restore before throwing
			Throw << "Failed to open include file: " << full_path.c_str() << flush;
		    }
		    source.fname(full_path.c_str());
		    source.copybuf(incf.rdbuf());
		    TokenBase *itb;
		    const char *_interned2 = intern_file(full_path);
		    while ( (itb = getRealToken()) )
		    {
			itb->file = _interned2;
			push_token_with_string_concat(itb);
		    }
		    source = std::move(saved);
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
		    // dlopen the library
		    void *handle = dlopen(libname.c_str(), RTLD_LAZY | RTLD_GLOBAL);
		    if ( !handle )
		    {
			std::string err = "Failed to load library: " + libname + ": " + dlerror();
			Throw << err.c_str() << flush;
		    }
		    dlopen_map[ns_name] = handle;
		    namespace_map[ns_name]; // create empty namespace
		    DBG(std::cout << "#load \"" << libname << "\" as " << ns_name << std::endl);
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
		    DBG(std::cout << "#undef " << name << std::endl);
		    // consume rest of line
		    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			source.get();
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
		    bool active = (directive == "ifdef") ? defined : !defined;
		    ifdef_stack.push(active);
		    ifdef_done_stack.push(active);
		    DBG(std::cout << "#" << directive << " " << name << " -> " << (active ? "true" : "false") << " stack=" << ifdef_stack.size() << " file=" << source.fname() << std::endl);
		    // consume rest of line
		    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			source.get();
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
		    // consume rest of line
		    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			source.get();
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
		    // consume rest of line
		    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			source.get();
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
			    // consume rest of line
			    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
				source.get();
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
				    std::string val = (it != define_map.end()) ? it->second : std::string("\x01");
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
			// consume rest of line
			while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			    source.get();
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

			TokenBase *tb = new TokenPREFER();
			tb->line = pragma_line;
			tb->column = pragma_col;
			injected_tokens.push_back(tb);
			for ( size_t i = 0; i < order.size(); ++i )
			{
			    TokenIdent *ti = new TokenIdent(order[i]);
			    ti->line = pragma_line;
			    ti->column = pragma_col;
			    injected_tokens.push_back(ti);
			    if ( i + 1 < order.size() )
			    {
				tb = new TokenComma();
				tb->line = pragma_line;
				tb->column = pragma_col;
				injected_tokens.push_back(tb);
			    }
			}
			tb = new TokenSemi();
			tb->line = pragma_line;
			tb->column = pragma_col;
			injected_tokens.push_back(tb);
		    }
		    else
		    {
			// consume rest of line for unknown pragmas
			while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			    source.get();
		    }
		    return _getToken();
		}
	    }
	    DBG(std::cout << "# fell through to TokenHash, peek=" << (int)source.peek() << " file=" << source.fname() << std::endl);
	    return new TokenHash;
	case '{': return new TokenOpBrc;
	case '}': return new TokenClBrc;
	case '(': return new TokenOpBrk;
	case ')': return new TokenClBrk;
	case '[':
	    // C23 [[attribute]] syntax: consume [[...]] and skip
	    if ( source.peek() == '[' )
	    {
		source.get(); // consume second '['
		int depth = 1;
		while ( source.good() && depth > 0 )
		{
		    char c = source.get();
		    if ( c == '[' ) ++depth;
		    else if ( c == ']' ) --depth;
		}
		return getToken();
	    }
	    return new TokenOpSqr;
	case ']': return new TokenClSqr;
	case '~': return new TokenBnot;
	case '!': if (source.peek() != '=') return new TokenLnot;		// !
	    source.get(); return new TokenNotEq;				// !=
	case '&':
	    if (source.peek() == '&') { source.get(); return new TokenLand;   }  // &&
	    if (source.peek() == '=') { source.get(); return new TokenBandEq; }  // &=
	    return new TokenBand;					// &
	case '|':
	    if (source.peek() == '|') { source.get(); return new TokenLor;    }  // ||
	    if (source.peek() == '=') { source.get(); return new TokenBorEq;  }  // |=
	    return new TokenBor;					// |
	case '%': if (source.peek() != '=') return new TokenMod;		// %
	    source.get(); return new TokenModEq;				// %=
	case '^': if (source.peek() != '=') return new TokenXor;		// ^
	     source.get(); return new TokenXorEq;				// ^=
	case '?': return new TokenTerQ;					// ?
	case ':':
	    if (source.peek() == ':') { source.get(); return new TokenNS; }   // ::
	    if (source.peek() == '=') { source.get(); return new TokenColEq; } // :=
	    return new TokenTerC;                                               // :
	case ';': return new TokenSemi;					// ,
	case ',': return new TokenComma;				// .
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
		return new TokenReal(num);
	    }
	    return new TokenDot;
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
	    return new TokenStr(word);
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
	    return new TokenChar(word[0]);
	case '<':
	    if (source.peek() == '=')
	    {
		source.get();
		if (source.peek() == '>') { source.get(); return new Token3Way; }  // <=>
		return new TokenLE;					  // <=
	    }
	    if (source.peek() == '<')
	    {
		source.get();
		if (source.peek() == '=') { source.get(); return new TokenBSLEq; } // <<=
		return new TokenBSL;					  // <<
	    }
	    return new TokenLT;						  // <
	case '>':
	    if (source.peek() == '=')     { source.get(); return new TokenGE;  }	  // >=
	    if (source.peek() == '>')
	    {
		source.get();
		if (source.peek() == '=') { source.get(); return new TokenBSREq; } // >>=
		return new TokenBSR;					  // >>
	    }
	    return new TokenGT;						  // >
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
		if ( is_binary_prefix(ch, source) )
		{
		    int64_t bv = read_binary_literal(source);
		    eat_int_suffix();
		    TokenInt *ti = new TokenInt(bv);
		    { DataDef *st = resolve_int_suffix_type(bv, true); if (st) ti->setDataType(st); }
		    // binary prefix source_text not critical for macro round-trip
		    return ti;
		}
		// hex literal: 0x... or 0X...
		if ( ch == '0' && source.good() && (source.peek() == 'x' || source.peek() == 'X') )
		{
		    lit_text += (char)source.get(); // eat 'x'/'X'
		    long long hv = 0;
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
		    }
		    if ( eat_imag_suffix() )
		    {
			TokenReal *tr = new TokenReal(strtod(lit_text.c_str(), NULL));
			char suffix = imag_type_suffix ? imag_type_suffix : real_type_suffix;
			tr->setDataType(get_complex_compat_type(complex_real_type_for_suffix(suffix)));
			return tr;
		    }
		    return new TokenReal(strtod(lit_text.c_str(), NULL));
		    }
		    eat_int_suffix();
		    {
			TokenInt *ti = new TokenInt((int64_t)hv);
			ti->source_text = lit_text;
			{ DataDef *st = resolve_int_suffix_type((int64_t)hv, true); if (st) ti->setDataType(st); }
			return ti;
		    }
		}
		// Octal literal: starts with 0, digits 0-7
		bool is_octal = (ch == '0') && source.good()
		    && source.peek() >= '0' && source.peek() <= '7';
		int64_t v = (ch & 0xf);

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
			v = v * 8 + (oc & 0xf);
		    }
		}
		else
		{
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
			    TokenReal *tr = new TokenReal(num);
			    char suffix = imag_type_suffix ? imag_type_suffix : real_type_suffix;
			    tr->setDataType(get_complex_compat_type(complex_real_type_for_suffix(suffix)));
			    return tr;
			}
			return new TokenReal(num);
		    }
		    if ( eat_imag_suffix() )
		    {
			TokenInt *ti = new TokenInt(v);
			ti->source_text = lit_text;
			ti->setDataType(get_complex_compat_type(&ddINT64));
			return ti;
		    }
		    eat_int_suffix();
		    TokenInt *ti = new TokenInt(v);
		    ti->source_text = lit_text;
		    { DataDef *st = resolve_int_suffix_type(v, is_octal); if (st) ti->setDataType(st); }
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
		}
		double num = strtod(lit_text.c_str(), NULL);
		if ( eat_imag_suffix() )
		{
		    TokenReal *tr = new TokenReal(num);
		    char suffix = imag_type_suffix ? imag_type_suffix : real_type_suffix;
		    tr->setDataType(get_complex_compat_type(complex_real_type_for_suffix(suffix)));
		    return tr;
		}
		return new TokenReal(num);
	    }
	    if ( ch == '_' || isalnum(ch) )
	    {
		word = "";
		word += ch;

		while ( source.good() && (isalnum(source.peek()) || source.peek() == '_') )
		    word += source.get();
		if ( word == "L" && source.good()
		  && (source.peek() == '"' || source.peek() == '\'') )
		    return getToken();
		// function-like macro expansion: NAME(args) or NAME (args)
		// Suppressed when the preceding tokens form a declaration /
		// definition head (`void bug(const char *, ...)` must not
		// be eaten by a prior `#define bug(...) ((void)0)`).
		if ( macro_map.count(word) && !source.macro_disabled(word)
		     && consume_macro_call_open(source) )
		{
		    MacroDef &macro = macro_map[word];
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
			return new TokenIdent(word);
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
				    expanded_arg += ((TokenIdent *)at)->str;
				    expanded_arg += '"';
				    break;
				case TokenType::ttChar:
				    expanded_arg += '\'';
				    expanded_arg += (char)at->get();
				    expanded_arg += '\'';
				    break;
				default:
				    if ( auto *ti = dynamic_cast<TokenIdent *>(at) )
					expanded_arg += ti->str;
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
		if ( define_map.count(word) && !source.macro_disabled(word) )
		{
		    std::string &val = define_map[word];
		    if ( !val.empty() )
		    {
			// Builtin libc aliases such as __builtin_strcmp -> strcmp
			// should resolve to the target identifier directly instead
			// of re-entering the macro rescanner. Otherwise user macros
			// like `#define strcmp __builtin_strcmp` recurse forever.
			if ( word.compare(0, 10, "__builtin_") == 0
			  && is_identifier_spelling(val)
			  && !builtin_alias_needs_retokenize(word, val) )
			    return new TokenIdent(val);
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
		    return new TokenIdent(word);
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
		      || gnu_attribute_text_has_name(attr_text, "no_instrument_function") )
		    {
			source.pushback(attr_text);
			return new TokenIdent(word);
		    }
		    return getToken();
		}
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
				source.pushback(std::string(" ") + w);
				break;
			    }
			}
			else
			{
			    // Not a type specifier — push it back
			    if ( !w.empty() )
				source.pushback(std::string(" ") + w);
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
				return new TokenDataType(complex_dd->name.c_str(), *complex_dd);
			    }
			    break;
			case TS_CHAR:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddCHAR); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("char", ddCHAR);
			case TS_SIGNED + TS_CHAR:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT8); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("signed char", ddINT8);
			case TS_UNSIGNED + TS_CHAR:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT8); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("unsigned char", ddUINT8);
			case TS_SHORT:
			case TS_SHORT + TS_INT:
			case TS_SIGNED + TS_SHORT:
			case TS_SIGNED + TS_SHORT + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT16); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("short", ddINT16);
			case TS_UNSIGNED + TS_SHORT:
			case TS_UNSIGNED + TS_SHORT + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT16); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("unsigned short", ddUINT16);
			case TS_INT:
			case TS_SIGNED:
			case TS_SIGNED + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT32); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("int", ddINT32);
			case TS_UNSIGNED:
			case TS_UNSIGNED + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT32); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("unsigned int", ddUINT32);
			case TS_LONG:
			case TS_LONG + TS_INT:
			case TS_SIGNED + TS_LONG:
			case TS_SIGNED + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT64); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("long", ddINT64);
			case TS_UNSIGNED + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT64); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("unsigned long", ddUINT64);
			case TS_LONG + TS_LONG:
			case TS_LONG + TS_LONG + TS_INT:
			case TS_SIGNED + TS_LONG + TS_LONG:
			case TS_SIGNED + TS_LONG + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT64); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("long long", ddINT64);
			case TS_UNSIGNED + TS_LONG + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_LONG + TS_INT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT64); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("unsigned long long", ddUINT64);
			case TS_INT128:
			case TS_SIGNED + TS_INT128:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddINT64); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("__int128", ddINT64);
			case TS_UNSIGNED + TS_INT128:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddUINT64); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("unsigned __int128", ddUINT64);
			case TS_FLOAT:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddFLOAT); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("float", ddFLOAT);
			case TS_DOUBLE:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddDOUBLE); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("double", ddDOUBLE);
			case TS_LONG + TS_DOUBLE:
			    if ( counter & TS_COMPLEX ) { DataDef *dd = get_complex_compat_type(&ddDOUBLE); return new TokenDataType(dd->name.c_str(), *dd); }
			    return new TokenDataType("long double", ddDOUBLE);
			default:
			    // Unrecognized combination — push back consumed words
			    // in reverse and fall through to identifier/keyword lookup.
			    for ( auto it = consumed.rbegin(); it != consumed.rend(); ++it )
				source.pushback(std::string(" ") + *it);
			    break;
		    }
		}
		if ( (kmi=keyword_map.find(word)) != keyword_map.end() )
		    return kmi->second->clone();
		if ( (bmi=datatype_map.find(word)) != datatype_map.end() )
		    return bmi->second->clone();
		if ( auto_include_standard_identifier(word) )
		    return getToken();
		return new TokenIdent(word);
	    }
	    return new TokenChar(ch);
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
	    // consume rest of line
	    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		source.get();
	}
	else if ( dir == "endif" )
	{
	    // consume rest of line
	    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		source.get();
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
	    // consume rest of line
	    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		source.get();
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
		// consume rest of line since we won't evaluate
		while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		    source.get();
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
		out += it->second.empty() ? "1" : it->second;
		changed = true;
	    }
	    else if ( macro_map.count(word) > 0 )
	    {
		// Function-like macro without args in #if context → treat as defined (1)
		out += "1";
		changed = true;
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
		if ( !it->second.empty() )
		    return strtoll(it->second.c_str(), NULL, 0);
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

    DBG(if (tb) printt(tb));
    return tb;
}

// get a real token (ignore whitespace and comments)
TokenBase *Program::getRealToken()
{
    TokenBase *tb;

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
		continue;
	    default:
		return tb;
	}
    }

    return NULL;
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
	    std::cout << ((TokenIdent *)tb)->str;
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
	    std::cout << ((TokenIdent *)tb)->str;
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttString:
	    if ( colors )
		std::cout << "\e[0;36m";
	    else
		std::cout << "STR::";
	    std::cout << '"' << ((TokenIdent *)tb)->str << '"';
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
	    std::cout << ((TokenIdent *)tb)->str;
	    if ( colors ) { std::cout << "\e[m"; }
	    break;
	case TokenType::ttDataType:
	    if ( colors )
		std::cout << "\e[1;37m";
	    else
		std::cout << "TYPE::";
	    std::cout << ((TokenIdent *)tb)->str;
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
	_ss.clear();

	if ( !row || !col )
	{
	    row = line();
	    col = column();
	}

	_cr = _lf = 0;
	_ss.seekg(0, _ss.beg);
	if ( !_ss.good() )
	    std::cerr << " seekfail";

	while ( peek() != -1 )
	{
	    getline(ln);
	    //cout << "line()-1 " << (line()-1) << "  row " << row << endl;
	    if ( line()-1 >= row )
		break;
        }

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

    source.fname(fname);
    source.copybuf(file.rdbuf());
    Throw.source(source);

    try
    {
	while ( (tb=getRealToken()) )
	{
	    tb->file = fname;
//	    tb->line = source.line();
//	    tb->column = source.column();
	    push_token_with_string_concat(tb);
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
	set_error(Program::DiagnosticPhase::lexer, std::string("use of undeclared identifier '") + ti->str + '\'', fname, source.line(), source.column());
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

    source.fname(fname);
    source.str(source_text);
    Throw.source(source);

    try
    {
	while ( (tb=getRealToken()) )
	{
	    tb->file = fname;
	    push_token_with_string_concat(tb);
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
		  std::string("use of undeclared identifier '") + ti->str + '\'',
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

    tkProgram->source = effective_name;
    tkProgram->is = new std::stringstream(source_text);
    tkProgram->lines = source.line()-1;
    tkProgram->bytes = source_text.size();

    return tkProgram;
}
