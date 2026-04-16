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
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;
using namespace asmjit;

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
TokenDEFAULT	tkDEFAULT;
TokenTYPEDEF	tkTYPEDEF;
TokenOPEROVER	tkOPEROVER;
TokenREGISTER	tkREGISTER;
TokenUSING	tkUSING;
TokenNAMESPACE	tkNAMESPACE;
TokenDEFER	tkDEFER;
TokenVECTOR	tkVECTOR;
TokenMAP	tkMAP;
TokenSET	tkSET;
TokenLIST	tkLIST;

// basic type tokens
TokenVOID	tkVOID;
TokenBOOL	tkBOOL;
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


void Program::_tokenizer_init()
{
    
    tkProgram = NULL;
    tkFunction = NULL;
    _cur_token = NULL;
    _prv_token = NULL;
    _include_iostream = false;
    _include_stdio = false;
    add_keywords();
    add_datatypes();
    struct_map["teststruct"] = &ddTESTSTRUCT;
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
    keyword_map[tkDEFAULT.str] = &tkDEFAULT;
    keyword_map[tkTYPEDEF.str] = &tkTYPEDEF;
    keyword_map[tkOPEROVER.str] = &tkOPEROVER;
    keyword_map[tkREGISTER.str] = &tkREGISTER;
    keyword_map[tkUSING.str] = &tkUSING;
    keyword_map[tkNAMESPACE.str] = &tkNAMESPACE;
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
			const std::string *embedded = find_embedded_header(incfile);
			if ( embedded )
			{
			    DBG(std::cout << "#include <" << incfile << "> (embedded)" << std::endl);
			    Source saved = std::move(source);
			    source = Source();
			    source.fname(incfile.c_str());
			    source.str(*embedded);
			    TokenBase *itb;
			    while ( (itb = getRealToken()) )
			    {
				itb->file = incfile.c_str();
				tokens.push_back(itb);
			    }
			    source = std::move(saved);
			    // flag headers for deferred registration during parse init
			    if ( incfile == "iostream" ) _include_iostream = true;
			    if ( incfile == "stdio.h" )  _include_stdio = true;
			    return getToken();
			}
		    }
		    // resolve relative path based on current file's directory
		    std::string full_path = incfile;
		    if ( !incfile.empty() && incfile[0] != '/' )
		    {
			std::string cur_fname(source.fname());
			size_t slash_pos = cur_fname.rfind('/');
			if ( slash_pos != std::string::npos )
			    full_path = cur_fname.substr(0, slash_pos + 1) + incfile;
		    }
		    DBG(std::cout << "#include \"" << full_path << "\"" << std::endl);
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
		    while ( (itb = getRealToken()) )
		    {
			itb->file = full_path.c_str();
			tokens.push_back(itb);
		    }
		    source = std::move(saved);
		    return getToken(); // continue with current file
		}
		if ( directive == "load" )
		{
		    // #load "libfoo.so" as namespace;
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
		    // #define NAME value
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    std::string name;
		    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
			name += source.get();
		    // skip whitespace between name and value
		    while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
		    // read value (rest of line, trimmed)
		    std::string value;
		    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
			value += source.get();
		    // trim trailing whitespace
		    while ( !value.empty() && (value.back() == ' ' || value.back() == '\t') )
			value.pop_back();
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
		    DBG(std::cout << "#undef " << name << std::endl);
		    // consume rest of line
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
		    bool defined = define_map.count(name) > 0;
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
	case '.': return new TokenDot;
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
			case '0':  word += '\0'; break;
			case 'a':  word += '\a'; break;
			case 'b':  word += '\b'; break;
			case 'f':  word += '\f'; break;
			case 'v':  word += '\v'; break;
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
			case '0':  word += '\0'; break;
			case 'a':  word += '\a'; break;
			case 'b':  word += '\b'; break;
			case 'f':  word += '\f'; break;
			case 'v':  word += '\v'; break;
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
		// hex literal: 0x... or 0X...
		if ( ch == '0' && source.good() && (source.peek() == 'x' || source.peek() == 'X') )
		{
		    source.get(); // eat 'x'
		    long long hv = 0;
		    while ( source.good() && isxdigit(source.peek()) )
		    {
			char hc = source.get();
			hv *= 16;
			if      ( hc >= '0' && hc <= '9' ) hv += hc - '0';
			else if ( hc >= 'a' && hc <= 'f' ) hv += hc - 'a' + 10;
			else                               hv += hc - 'A' + 10;
		    }
		    return new TokenInt((int)hv);
		}
		int v = (ch & 0xf);

		while ( source.good() && isdigit(source.peek()) )
		{
		    v *= 10;
		    v += source.get() & 0xf;
		}
		// no decimal means integer
		if ( source.peek() != '.' )
		    return new TokenInt(v);
		// handle floating point
		source.get(); // eat .
		double num = v, divisor = 10;
		while ( source.good() && isdigit(source.peek()) )
		{
		    num += (source.get() & 0xf) / divisor;
		    divisor *= 10;
		}
		return new TokenReal(num);
	    }
	    if ( ch == '_' || isalnum(ch) )
	    {
		word = "";
		word += ch;

		while ( source.good() && (isalnum(source.peek()) || source.peek() == '_') )
		    word += source.get();
		// #define substitution: inject the define value into the source stream
		if ( define_map.count(word) )
		{
		    std::string &val = define_map[word];
		    if ( !val.empty() )
		    {
			source.pushback(val);
			return getToken(); // re-tokenize the substituted text
		    }
		    // empty define — skip and get next token
		    return getToken();
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
	// skip to next '#' at start of a directive
	char ch = source.get();
	if ( ch == '\n' || ch == '\r' )
	    continue;
	if ( ch != '#' )
	{
	    // skip rest of line
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

// evaluate #if condition: supports "defined(NAME)", "!defined(NAME)", integer constants
bool Program::evaluateIfCondition()
{
    // skip whitespace
    while ( source.peek() == ' ' || source.peek() == '\t' )
	source.get();

    bool negate = false;
    if ( source.peek() == '!' )
    {
	source.get();
	negate = true;
	while ( source.peek() == ' ' || source.peek() == '\t' )
	    source.get();
    }

    // check for "defined" keyword
    if ( isalpha(source.peek()) )
    {
	std::string word;
	while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
	    word += source.get();

	if ( word == "defined" )
	{
	    while ( source.peek() == ' ' || source.peek() == '\t' )
		source.get();
	    bool has_paren = false;
	    if ( source.peek() == '(' )
	    {
		source.get();
		has_paren = true;
		while ( source.peek() == ' ' || source.peek() == '\t' )
		    source.get();
	    }
	    std::string name;
	    while ( source.good() && !source.eof() && (isalnum(source.peek()) || source.peek() == '_') )
		name += source.get();
	    if ( has_paren )
	    {
		while ( source.peek() == ' ' || source.peek() == '\t' )
		    source.get();
		if ( source.peek() == ')' )
		    source.get();
	    }
	    // consume rest of line
	    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
		source.get();
	    bool result = define_map.count(name) > 0;
	    return negate ? !result : result;
	}
	// plain identifier — check if it's defined and non-zero
	// consume rest of line
	while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
	    source.get();
	bool result = define_map.count(word) > 0;
	if ( result )
	{
	    std::string &val = define_map[word];
	    if ( !val.empty() )
		result = (atoi(val.c_str()) != 0);
	}
	return negate ? !result : result;
    }

    // integer constant: #if 0, #if 1
    if ( isdigit(source.peek()) )
    {
	int val = 0;
	while ( source.good() && isdigit(source.peek()) )
	{
	    val *= 10;
	    val += source.get() - '0';
	}
	// consume rest of line
	while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
	    source.get();
	bool result = (val != 0);
	return negate ? !result : result;
    }

    // consume rest of line for anything we don't understand
    while ( source.good() && !source.eof() && source.peek() != '\n' && source.peek() != '\r' )
	source.get();
    return negate ? true : false;
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
	tb->line = source.line(); //_line;
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

    if ( !file )
    {
	cerr << "Failed to open " << fname << endl;
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
	    tokens.push_back(tb);
        }
    }
    catch(const char *err_msg)
    {
	cerr << ANSI_WHITE << fname << ':' << source.line() << ':' << source.column() 
	     << ": \e[1;31merror:\e[1;37m " << err_msg << ANSI_RESET << endl;
	source.showerror(source.line(), source.column());
	return NULL;
    }
    catch(TokenIdent *ti)
    {
	cerr << ANSI_WHITE << fname << ':' << source.line() << ':' << source.column()
	     << ": \e[1;31merror:\e[1;37m use of undeclared identifier '" << ti->str << '\'' << ANSI_RESET << endl;
	source.showerror(source.line(), source.column());
	return NULL;
    }
    catch(TokenBase *tb)
    {
	cerr << ANSI_WHITE << fname << ':' << source.line() << ':' << source.column()
	     << ": \e[1;31merror:\e[1;37m unexpected token type " << (int)tb->type() << ANSI_RESET << endl;
	source.showerror(source.line(), source.column());
	return NULL;
    }
    catch(std::exception &e)
    {
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
