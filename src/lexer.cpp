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
#define DBG(x) x
#include <asmjit/asmjit.h>
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
TokenLPSTR	tkLPSTR;


void Program::_tokenizer_init()
{
    
    tkProgram = NULL;
    tkFunction = NULL;
    _cur_token = NULL;
    _prv_token = NULL;
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
    datatype_map[tkLPSTR.str] = &tkLPSTR;
}


// lex and return the next token from the data stream
// TODO: replace top switch with direct dispatch
//       also likely better to replace istream stuff
//       with a character buffer for maximum speed
TokenBase *Program::_getToken(istream &ss)
{
    keyword_map_iter kmi;
    datatype_map_iter bmi;
    string word;
    int ch, cnt;

    if ( !ss.good() || ss.eof() ) { return NULL; }

    switch( (ch=get(ss)) )
    {
	case ' ':
	    cnt = 1;
	    while ( ss.peek() == ' ' )
	    {
		++cnt;
		get(ss);
		if ( !ss.good() || ss.eof() )
		    break;
	    }
	    return new TokenSpace(cnt);
	case '\t':
	    cnt = 1;
	    while ( ss.peek() == '\t' )
	    {
		++cnt;
		get(ss);
		if ( !ss.good() || ss.eof() )
		    break;
	    }
	    return new TokenTab(cnt);
	case '\r':
	    get(ss);
	case '\n':
	    cnt = 1;
	    while ( ss.peek() == '\r' || ss.peek() == '\n' )
	    {
		++cnt;
		if ( ss.peek() == '\r' ) { get(ss); }
		get(ss);
		if ( !ss.good() || ss.eof() )
		    break;
	    }
	    _column = 0;
	    _line += cnt;
	    _pos = ss.tellg();
	    return new TokenEOL(cnt);
	case '=':
	    if (ss.peek() == '=')
	    {
		get(ss);
		if (ss.peek() == '=') { get(ss); return new Token3Eq; } // ===
		return new TokenEquals;					// ==
	    }
	    return new TokenAssign;					// =
	case '+':
	    if (ss.peek() == '+') { get(ss); return new TokenInc;   }   // ++
	    if (ss.peek() == '=') { get(ss); return new TokenAddEq; }   // +=
	    return new TokenAdd;					// +
	case '-':
	    if (ss.peek() == '-') { get(ss); return new TokenDec;   }   // --
	    if (ss.peek() == '=') { get(ss); return new TokenSubEq; }   // -=
	    if (ss.peek() == '>') { get(ss); return new TokenDeRef; }   // ->
	    return new TokenNeg;					// -
	case '*': if (ss.peek() != '=') return new TokenMul;		// *
	     get(ss); return new TokenMulEq;				// *=
	case '/':
	    if (ss.peek() == '=') { get(ss); return new TokenDivEq; }   // /=
	    if (ss.peek() == '/')					// //
	    {
		get(ss);
		word = "//";
		while ( ss.good() && !ss.eof() && ss.peek() != '\r' && ss.peek() != '\n' )
		    word += get(ss);
		return new TokenREM(word);
	    }
	    if (ss.peek() == '*')					// /*
	    {
		get(ss);
		word = "/*";
		while ( ss.good() && !ss.eof() )
		{
		    ch = get(ss);
		    if ( ch == '*' && ss.peek() == '/' )		// */
		    {
			word += ch;
			word += get(ss);
			break;
		    }
		    word += ch;
		}
		return new TokenREM(word);
	    }
	    return new TokenDiv;
	case '\\': return new TokenBslsh;
	case '#': // #! is a special comment style for shell script execution
	    if ( ss.peek() == '!' )
	    {
		get(ss);
		word = "#!";
		while ( ss.good() && !ss.eof() && ss.peek() != '\r' && ss.peek() != '\n' )
		    word += get(ss);
		return new TokenREM(word);
	    }
	    return new TokenHash;
	case '{': return new TokenOpBrc;
	case '}': return new TokenClBrc;
	case '(': return new TokenOpBrk;
	case ')': return new TokenClBrk;
	case '[': return new TokenOpSqr;
	case ']': return new TokenClSqr;
	case '~': return new TokenBnot;
	case '!': if (ss.peek() != '=') return new TokenLnot;		// !
	    get(ss); return new TokenNotEq;				// !=
	case '&':
	    if (ss.peek() == '&') { get(ss); return new TokenLand;   }  // &&
	    if (ss.peek() == '=') { get(ss); return new TokenBandEq; }  // &=
	    return new TokenBand;					// &
	case '|':
	    if (ss.peek() == '|') { get(ss); return new TokenLor;    }  // ||
	    if (ss.peek() == '=') { get(ss); return new TokenBorEq;  }  // |=
	    return new TokenBor;					// |
	case '%': if (ss.peek() != '=') return new TokenMod;		// %
	    get(ss); return new TokenModEq;				// %=
	case '^': if (ss.peek() != '=') return new TokenXor;		// ^
	     get(ss); return new TokenXorEq;				// ^=
	case '?': return new TokenTerQ;					// ?
	case ':': if (ss.peek() != ':') return new TokenTerC;		// :
	    get(ss); return new TokenNS;				// ::
	case ';': return new TokenSemi;					// ,
	case ',': return new TokenComma;				// .
	case '.': return new TokenDot;
	case '"':
	    word = "";
	    while ( ss.good() && ss.peek() != '"' )
	    {
		if ( ss.peek() == '\\' )
		    word += get(ss);
		word += get(ss);
	    }
	    if ( !ss.good() )
		throw "Unterminated string";
	    get(ss);
	    return new TokenStr(word);
	case '\'':
	    word = "";
	    while ( ss.good() && ss.peek() != '\'' )
	    {
		if ( ss.peek() == '\\' )
		    get(ss);
		word += get(ss);
	    }
	    if ( !ss.good() )
		throw "Unterminated string";
	    get(ss);
	    return new TokenChar(word[0]);
	case '<':
	    if (ss.peek() == '=')
	    {
		get(ss);
		if (ss.peek() == '>') { get(ss); return new Token3Way; }  // <=>
		return new TokenLE;					  // <=
	    }
	    if (ss.peek() == '<')
	    {
		get(ss);
		if (ss.peek() == '=') { get(ss); return new TokenBSLEq; } // <<=
		return new TokenBSL;					  // <<
	    }
	    return new TokenLT;						  // <
	case '>':
	    if (ss.peek() == '=')     { get(ss); return new TokenGE;  }	  // >=
	    if (ss.peek() == '>')
	    {
		get(ss);
		if (ss.peek() == '=') { get(ss); return new TokenBSREq; } // >>=
		return new TokenBSR;					  // >>
	    }
	    return new TokenGT;						  // >
	default:
	    if ( isdigit(ch) )
	    {
		int v = (ch & 0xf);
		
		while ( ss.good() && isdigit(ss.peek()) )
		{
		    v *= 10;
		    v += get(ss) & 0xf;
		}
		// no decimal means integer
		if ( ss.peek() != '.' )		
		    return new TokenInt(v);
		// handle floating point
		get(ss); // eat .
		double num = v, divisor = 10;
		while ( ss.good() && isdigit(ss.peek()) )
		{
		    num += (get(ss) & 0xf) / divisor;
		    divisor *= 10;
		}
		return new TokenReal(num);
	    }
	    if ( ch == '_' || isalnum(ch) )
	    {
		word = "";
		word += ch;

		while ( ss.good() && (isalnum(ss.peek()) || ss.peek() == '_') )
		    word += get(ss);
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

TokenBase *Program::getToken(istream &ss)
{
    TokenBase *tb = _getToken(ss);

    DBG(if (tb) printt(tb));
    return tb;
}

// get a real token (ignore whitespace and comments)
TokenBase *Program::getRealToken(istream &ss)
{
    TokenBase *tb;

    while ( (tb=getToken(ss)) )
    {
	tb->line = _line;
	tb->column = _column;
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

#if 0
// tokenize stream of data TODO -- do all the same as tokenize(file), except set up filename
void Program::tokenize(istream &ss)
{
    TokenBase *tb = NULL;

    DBG(std::cout << "Program::parse()" << std::endl << std::endl);

    _init();

    while ( (tb=getToken(ss)) )
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

    _pos = 0;
    _line = 1;
    _column = 0;

    try
    {
	while ( (tb=getRealToken(file)) )
	{
	    tb->file = fname;
	    tb->line = _line;
	    tb->column = _column;
	    tokens.push(tb);
        }
    }
    catch(const char *err_msg)
    {
	cerr << ANSI_WHITE << fname << ':' << _line << ':' << _column 
	     << ": \e[1;31merror:\e[1;37m " << err_msg << ANSI_RESET << endl;
	showerror(file);
	return NULL;
    }
    catch(TokenIdent *ti)
    {
	cerr << ANSI_WHITE << fname << ':' << _line << ':' << _column
	     << ": \e[1;31merror:\e[1;37m use of undeclared identifier '" << ti->str << '\'' << ANSI_RESET << endl;
	showerror(file);
	return NULL;
    }
    catch(TokenBase *tb)
    {
	cerr << ANSI_WHITE << fname << ':' << _line << ':' << _column
	     << ": \e[1;31merror:\e[1;37m unexpected token type " << (int)tb->type() << ANSI_RESET << endl;
	showerror(file);
	return NULL;
    }

    DBG(std::cout << "Program::tokenize() finished tokenizing" << std::endl);

    tkProgram = new TokenProgram();
    tkFunction = tkProgram;

    file.clear();

    tkProgram->source = fname;
    tkProgram->is = new ifstream(fname);
    tkProgram->lines = _line-1;
    tkProgram->bytes = file.tellg();

    return tkProgram;
}
