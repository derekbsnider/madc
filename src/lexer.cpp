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

    while ( source.good() && (source.peek() == ' ' || source.peek() == '\t') )
	spacing += source.get();
    if ( source.peek() == '(' )
    {
	source.get(); // consume '('
	return true;
    }
    if ( !spacing.empty() )
	source.pushback(spacing);
    return false;
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
    included_files.clear();
    add_keywords();
    add_datatypes();
    struct_map["teststruct"] = &ddTESTSTRUCT;

    // Ignored C qualifiers — consumed as empty defines so they
    // don't trip the "undeclared identifier" path.
    define_map["volatile"] = "";
    define_map["__volatile__"] = "";
    define_map["inline"] = "";
    define_map["__inline__"] = "";
    define_map["__inline"] = "";
    define_map["__extension__"] = "";
    define_map["__restrict"] = "";
    define_map["__restrict__"] = "";
    define_map["__signed__"] = "signed";
    define_map["__const"] = "const";
    define_map["__const__"] = "const";
    // C99 _Complex / GCC __complex__ — not supported but at least
    // don't error on the keyword. Map to double (loses the imaginary
    // part but lets real-only code compile).
    define_map["_Complex"] = "";
    define_map["__complex__"] = "";

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
    define_map["__SHRT_MAX__"] = "32767";
    define_map["__SCHAR_MAX__"] = "127";
    define_map["__PTRDIFF_TYPE__"] = "long";
    define_map["__SIZE_TYPE__"] = "unsigned long";
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
    define_map["__builtin_strchr"] = "strchr";
    define_map["__builtin_strrchr"] = "strrchr";
    define_map["__builtin_strstr"] = "strstr";
    define_map["__builtin_strncat"] = "strncat";
    define_map["__builtin_strcat"] = "strcat";
    define_map["__builtin_snprintf"] = "snprintf";
    define_map["__builtin_fprintf"] = "fprintf";
    define_map["__builtin_sscanf"] = "sscanf";
    define_map["__builtin_fscanf"] = "fscanf";
    define_map["__builtin_realloc"] = "realloc";
    define_map["__builtin_alloca"] = "alloca";
    define_map["__builtin_bzero"] = "bzero";
    define_map["__builtin_bcopy"] = "bcopy";

    // __builtin_expect(expr, val) is a branch-prediction hint — just
    // return expr. Implemented as a function-like macro.
    {
	MacroDef m;
	m.params = {"__expr", "__val"};
	m.body = "__expr";
	macro_map["__builtin_expect"] = m;
    }
    // __builtin_prefetch is a no-op hint
    {
	MacroDef m;
	m.params = {"__addr"};
	m.body = "((void)0)";
	m.variadic = true;
	macro_map["__builtin_prefetch"] = m;
    }
    // __builtin_constant_p(expr) — always return 0 (not a constant)
    {
	MacroDef m;
	m.params = {"__expr"};
	m.body = "0";
	macro_map["__builtin_constant_p"] = m;
    }
    // __builtin_unreachable() — map to abort()
    define_map["__builtin_unreachable"] = "abort";
    define_map["__builtin_alloca"] = "malloc"; // alloca = stack alloc, map to malloc for now
    define_map["__builtin_ffs"] = "ffs";
    define_map["__builtin_bswap32"] = "__bswap_32";
    define_map["__builtin_bswap64"] = "__bswap_64";
    define_map["__builtin_clz"] = "__builtin_clz"; // will fall through to dlsym
    define_map["__builtin_ctz"] = "__builtin_ctz";

    // __builtin_offsetof(type, member) — compute struct member offset.
    // Implemented as a function-like macro using the null-pointer trick.
    {
	MacroDef m;
	m.params = {"__type", "__member"};
	m.body = "((long)&((__type *)0)->__member)";
	macro_map["__builtin_offsetof"] = m;
    }

    // __builtin_types_compatible_p(t1, t2) — always return 0 for now
    {
	MacroDef m;
	m.params = {"__t1", "__t2"};
	m.body = "0";
	macro_map["__builtin_types_compatible_p"] = m;
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

    if ( is_system )
	return incfile;

    std::string cur_dir = current_source_directory();
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
    keyword_map[tkUSING.str] = &tkUSING;
    keyword_map[tkNAMESPACE.str] = &tkNAMESPACE;
    keyword_map[tkPREFER.str] = &tkPREFER;
    keyword_map[tkDEFER.str] = &tkDEFER;
    keyword_map[tkVECTOR.str] = &tkVECTOR;
    keyword_map[tkMAP.str] = &tkMAP;
    keyword_map[tkSET.str] = &tkSET;
    keyword_map[tkLIST.str] = &tkLIST;
}

// add static tokens for base data types
void Program::add_datatypes()
{
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
    datatype_map[tkSTRING.str] = &tkSTRING;
    datatype_map[tkSSTREAM.str] = &tkSSTREAM;
    datatype_map[tkARRAY.str] = &tkARRAY;
    datatype_map[tkIFSTREAM.str] = &tkIFSTREAM;
    datatype_map[tkOFSTREAM.str] = &tkOFSTREAM;
    datatype_map[tkFSTREAM.str] = &tkFSTREAM;
    datatype_map[tkLPSTR.str] = &tkLPSTR;
    datatype_map[tkAUTO.str] = &tkAUTO;
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

    switch( (ch=source.get()) )
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
			    if ( incfile == "iostream" ) _include_iostream = true;
			    if ( incfile == "stdio.h" )  _include_stdio = true;
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
			    if ( !param.empty() )
				macro.params.push_back(param);
			    else
				Throw << "Expecting macro parameter name" << flush;
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
		    DBG(std::cout << "#" << directive << " " << name << " -> " << (active ? "true" : "false") << std::endl);
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
		    DBG(std::cout << "#else -> " << (active ? "true" : "false") << std::endl);
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
		if ( directive == "pragma" )
		{
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string pragma;
		    int pragma_line = source.line();
		    int pragma_col = source.column();
		    while ( source.good() && !source.eof() && isalpha(source.peek()) )
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
	    return new TokenHash;
	case '{': return new TokenOpBrc;
	case '}': return new TokenClBrc;
	case '(': return new TokenOpBrk;
	case ')': return new TokenClBrk;
	case '[': return new TokenOpSqr;
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
		// Consume C integer-literal suffixes (u/U, l/L, ll/LL, combos
		// like ul/ULL/Lu) and return true when the U flag is present.
		// madc's int is 64-bit so the L/LL size hints are
		// informational only, but signedness (U) matters for bitwise
		// operators and comparisons.
		bool has_u_suffix = false;
		auto eat_int_suffix = [&]() {
		    for (int i = 0; i < 3; ++i)
		    {
			int c = source.peek();
			if (c == 'u' || c == 'U')
			{
			    has_u_suffix = true;
			    source.get();
			}
			else if (c == 'l' || c == 'L')
			    source.get();
			else
			    break;
		    }
		};
		if ( is_binary_prefix(ch, source) )
		{
		    int64_t bv = read_binary_literal(source);
		    eat_int_suffix();
		    TokenInt *ti = new TokenInt(bv);
		    if (has_u_suffix) ti->setDataType(&ddUINT32);
		    return ti;
		}
		// hex literal: 0x... or 0X...
		if ( ch == '0' && source.good() && (source.peek() == 'x' || source.peek() == 'X') )
		{
		    source.get(); // eat 'x'
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
			hv *= 16;
			if      ( hc >= '0' && hc <= '9' ) hv += hc - '0';
			else if ( hc >= 'a' && hc <= 'f' ) hv += hc - 'a' + 10;
			else                               hv += hc - 'A' + 10;
		    }
		    eat_int_suffix();
		    {
			TokenInt *ti = new TokenInt((int64_t)hv);
			if (has_u_suffix) ti->setDataType(&ddUINT32);
			return ti;
		    }
		}
		int64_t v = (ch & 0xf);

		while ( source.good() )
		{
		    if ( is_digit_separator(source.peek()) )
		    {
			source.get();
			continue;
		    }
		    if ( !isdigit(source.peek()) )
			break;
		    v *= 10;
		    v += source.get() & 0xf;
		}
		// no decimal means integer — unless followed by e/E (scientific)
		if ( source.peek() != '.' )
		{
		    if ( source.peek() == 'e' || source.peek() == 'E' )
		    {
			// Scientific notation without decimal: 1e5, 2E-3
			source.get(); // consume e/E
			double num = (double)v;
			int exp_sign = 1;
			if ( source.good() && (source.peek() == '+' || source.peek() == '-') )
			{
			    if ( source.peek() == '-' ) exp_sign = -1;
			    source.get();
			}
			int exp_val = 0;
			while ( source.good() && isdigit(source.peek()) )
			    exp_val = exp_val * 10 + (source.get() & 0xf);
			double factor = 1.0;
			for ( int i = 0; i < exp_val; ++i ) factor *= 10.0;
			if ( exp_sign > 0 ) num *= factor; else num /= factor;
			if ( source.good() )
			{
			    int c = source.peek();
			    if ( c == 'f' || c == 'F' || c == 'l' || c == 'L' )
				source.get();
			}
			return new TokenReal(num);
		    }
		    eat_int_suffix();
		    TokenInt *ti = new TokenInt(v);
		    if (has_u_suffix) ti->setDataType(&ddUINT32);
		    return ti;
		}
		// handle floating point
		source.get(); // eat .
		double num = v, divisor = 10;
		while ( source.good() )
		{
		    if ( is_digit_separator(source.peek()) )
		    {
			source.get();
			continue;
		    }
		    if ( !isdigit(source.peek()) )
			break;
		    num += (source.get() & 0xf) / divisor;
		    divisor *= 10;
		}
		// Scientific notation exponent: e/E followed by optional +/- and digits
		if ( source.good() && (source.peek() == 'e' || source.peek() == 'E') )
		{
		    source.get(); // consume e/E
		    int exp_sign = 1;
		    if ( source.good() && (source.peek() == '+' || source.peek() == '-') )
		    {
			if ( source.peek() == '-' ) exp_sign = -1;
			source.get();
		    }
		    int exp_val = 0;
		    while ( source.good() && isdigit(source.peek()) )
		    {
			exp_val = exp_val * 10 + (source.get() & 0xf);
		    }
		    double factor = 1.0;
		    for ( int i = 0; i < exp_val; ++i )
			factor *= 10.0;
		    if ( exp_sign > 0 ) num *= factor;
		    else num /= factor;
		}
		// C float literal suffixes (f/F, l/L). f marks a float (4-byte
		// real); madc doesn't currently distinguish float-vs-double
		// literals at lex time (TokenReal is always double-precision),
		// so we just consume the suffix char.
		if ( source.good() )
		{
		    int c = source.peek();
		    if ( c == 'f' || c == 'F' || c == 'l' || c == 'L' )
			source.get();
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
		     && !looks_like_decl_head(tokens)
		     && consume_macro_call_open(source) )
		{
		    MacroDef &macro = macro_map[word];
		    // read actual arguments (handling nested parens and strings)
		    std::vector<std::string> args;
		    std::string arg;
		    int depth = 1;
		    while ( source.good() && depth > 0 )
		    {
			char mc = source.get();
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
		    // last argument
		    while ( !arg.empty() && (arg.front() == ' ' || arg.front() == '\t') ) arg.erase(arg.begin());
		    while ( !arg.empty() && (arg.back() == ' ' || arg.back() == '\t') ) arg.pop_back();
		    if ( !arg.empty() || !args.empty() )
			args.push_back(arg);
		    // substitute params in body — single pass over the
		    // original body so an argument that happens to match a
		    // later parameter name doesn't get re-substituted. A
		    // per-param sequential sweep cascades: for
		    // `CREATE(type, T, 1)` where the user's variable is
		    // named `type` (macro's first arg), substituting
		    // `result→type` first and `type→T` next would rewrite
		    // the user's variable and corrupt the expansion.
		    std::map<std::string, const std::string *> param_map;
		    for ( size_t i = 0; i < macro.params.size() && i < args.size(); ++i )
			param_map[macro.params[i]] = &args[i];
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
			    if ( it != param_map.end() )
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
			for ( size_t i = macro.params.size(); i < args.size(); ++i )
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
		// Most GCC attributes are no-ops for madc. Preserve `packed`
		// so the struct parser can select packed layout; skip the rest.
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
		    if ( attr_text.find("packed") != std::string::npos
		      || attr_text.find("aligned") != std::string::npos )
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
		if ( word == "unsigned" || word == "signed" || word == "long"
		  || word == "short"   || word == "int"    || word == "char"
		  || word == "double"  || word == "float" )
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
		    };
		    auto word_to_flag = [](const std::string &w) -> int {
			if (w == "char")     return TS_CHAR;
			if (w == "short")    return TS_SHORT;
			if (w == "int")      return TS_INT;
			if (w == "long")     return TS_LONG;
			if (w == "float")    return TS_FLOAT;
			if (w == "double")   return TS_DOUBLE;
			if (w == "signed")   return TS_SIGNED;
			if (w == "unsigned") return TS_UNSIGNED;
			return 0;
		    };
		    int counter = word_to_flag(word);
		    // Accumulate subsequent type-specifier keywords
		    auto read_word = [&]() -> std::string {
			while ( source.good() && (source.peek() == ' ' || source.peek() == '\t') )
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
			int flag = word_to_flag(w);
			if ( flag )
			{
			    counter += flag;
			    consumed.push_back(w);
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
		    switch ( counter )
		    {
			case TS_CHAR:                                   return new TokenDataType("char", ddCHAR);
			case TS_SIGNED + TS_CHAR:                       return new TokenDataType("signed char", ddINT8);
			case TS_UNSIGNED + TS_CHAR:                     return new TokenDataType("unsigned char", ddUINT8);
			case TS_SHORT:
			case TS_SHORT + TS_INT:
			case TS_SIGNED + TS_SHORT:
			case TS_SIGNED + TS_SHORT + TS_INT:             return new TokenDataType("short", ddINT16);
			case TS_UNSIGNED + TS_SHORT:
			case TS_UNSIGNED + TS_SHORT + TS_INT:           return new TokenDataType("unsigned short", ddUINT16);
			case TS_INT:
			case TS_SIGNED:
			case TS_SIGNED + TS_INT:                        return new TokenDataType("int", ddINT32);
			case TS_UNSIGNED:
			case TS_UNSIGNED + TS_INT:                      return new TokenDataType("unsigned int", ddUINT32);
			case TS_LONG:
			case TS_LONG + TS_INT:
			case TS_SIGNED + TS_LONG:
			case TS_SIGNED + TS_LONG + TS_INT:              return new TokenDataType("long", ddINT64);
			case TS_UNSIGNED + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_INT:            return new TokenDataType("unsigned long", ddUINT64);
			case TS_LONG + TS_LONG:
			case TS_LONG + TS_LONG + TS_INT:
			case TS_SIGNED + TS_LONG + TS_LONG:
			case TS_SIGNED + TS_LONG + TS_LONG + TS_INT:   return new TokenDataType("long long", ddINT64);
			case TS_UNSIGNED + TS_LONG + TS_LONG:
			case TS_UNSIGNED + TS_LONG + TS_LONG + TS_INT:  return new TokenDataType("unsigned long long", ddUINT64);
			case TS_FLOAT:                                  return new TokenDataType("float", ddFLOAT);
			case TS_DOUBLE:                                 return new TokenDataType("double", ddDOUBLE);
			case TS_LONG + TS_DOUBLE:                       return new TokenDataType("long double", ddDOUBLE);
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
	    if ( depth == 0 )
	    {
		// this #endif closes our block
		ifdef_stack.pop();
		ifdef_done_stack.pop();
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
bool Program::evaluateIfCondition()
{
    std::string expr;
    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
	expr += source.get();

    size_t pos = 0;

    auto skip_ws = [&]() {
	while ( pos < expr.size() && (expr[pos] == ' ' || expr[pos] == '\t') )
	    ++pos;
    };

    std::function<bool()> parse_or;
    std::function<bool()> parse_and;
    std::function<bool()> parse_unary;
    std::function<bool()> parse_primary;

    parse_primary = [&]() -> bool {
	skip_ws();
	if ( pos >= expr.size() )
	    return false;

	if ( expr[pos] == '(' )
	{
	    ++pos;
	    bool value = parse_or();
	    skip_ws();
	    if ( pos < expr.size() && expr[pos] == ')' )
		++pos;
	    return value;
	}

	if ( isdigit((unsigned char)expr[pos]) )
	{
	    int val = 0;
	    while ( pos < expr.size() && isdigit((unsigned char)expr[pos]) )
	    {
		val *= 10;
		val += expr[pos++] - '0';
	    }
	    return val != 0;
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
		return define_map.count(name) > 0 || macro_map.count(name) > 0;
	    }

	    bool result = define_map.count(word) > 0 || macro_map.count(word) > 0;
	    if ( result )
	    {
		std::string &val = define_map[word];
		if ( !val.empty() )
		    result = (atoi(val.c_str()) != 0);
	    }
	    return result;
	}

	return false;
    };

    parse_unary = [&]() -> bool {
	skip_ws();
	if ( pos < expr.size() && expr[pos] == '!' )
	{
	    ++pos;
	    return !parse_unary();
	}
	return parse_primary();
    };

    parse_and = [&]() -> bool {
	bool value = parse_unary();
	for (;;)
	{
	    skip_ws();
	    if ( pos + 1 < expr.size() && expr[pos] == '&' && expr[pos + 1] == '&' )
	    {
		pos += 2;
		value = value && parse_unary();
		continue;
	    }
	    return value;
	}
    };

    parse_or = [&]() -> bool {
	bool value = parse_and();
	for (;;)
	{
	    skip_ws();
	    if ( pos + 1 < expr.size() && expr[pos] == '|' && expr[pos + 1] == '|' )
	    {
		pos += 2;
		value = value || parse_and();
		continue;
	    }
	    return value;
	}
    };

    return parse_or();
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
