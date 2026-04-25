//////////////////////////////////////////////////////////////////////////
//									//
// madc parser methods to parse the tokens into an AST			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <set>
#include <vector>
#include <functional>
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

static bool is_restrict_token(TokenBase *tb)
{
    return tb && tb->type() == TokenType::ttKeyword && tb->id() == TokenID::tkRESTRICT;
}

static bool is_post_pointer_qualifier_token(TokenBase *tb)
{
    return tb && tb->type() == TokenType::ttKeyword
        && (tb->id() == TokenID::tkRESTRICT || tb->id() == TokenID::tkCONST);
}

static bool is_contextual_identifier_token(TokenBase *tb);
static std::string contextual_identifier_name(TokenBase *tb);

static bool read_constant_integer(Variable *var, int64_t &out)
{
    if ( !var || !var->is_constant() || !var->data || !var->type || !var->type->is_integer() )
	return false;
    switch ( var->type->rawtype() )
    {
	case DataType::dtINT8:   out = *((int8_t *)var->data);   return true;
	case DataType::dtUINT8:  out = *((uint8_t *)var->data);  return true;
	case DataType::dtINT16:  out = *((int16_t *)var->data);  return true;
	case DataType::dtUINT16: out = *((uint16_t *)var->data); return true;
	case DataType::dtINT24:  out = *((int16_t *)var->data);  return true;
	case DataType::dtUINT24: out = *((uint16_t *)var->data); return true;
	case DataType::dtINT32:  out = *((int32_t *)var->data);  return true;
	case DataType::dtUINT32: out = *((uint32_t *)var->data); return true;
	case DataType::dtINT64:  out = *((int64_t *)var->data);  return true;
	case DataType::dtUINT64: out = *((uint64_t *)var->data); return true;
	default:
	    return false;
    }
}

static bool resolve_integer_constant(Program &pgm, TokenBase *tb, int64_t &out)
{
    if ( !tb )
	return false;
    if ( tb->type() == TokenType::ttInteger )
    {
	out = ((TokenInt *)tb)->get();
	return true;
    }
    if ( tb->type() == TokenType::ttChar )
    {
	out = ((TokenChar *)tb)->get();
	return true;
    }
    // Also accept contextual-identifier keywords (`class`, `try`, `catch`,
    // etc.) as integer constants when they resolve to one — this lets
    // `case class:` work for an enum that has `class` as one of its tags.
    std::string name;
    if ( tb->type() == TokenType::ttIdentifier )
	name = ((TokenIdent *)tb)->str;
    else if ( is_contextual_identifier_token(tb) )
	name = contextual_identifier_name(tb);
    else
	return false;
    Variable *var = pgm.findVariable(name);
    return read_constant_integer(var, out);
}

static int64_t parse_constant_integer_expression(Program &pgm);

static TokenBase *parse_parenthesized_expression(Program &pgm, const char *context,
						 bool stop_on_closing_paren)
{
    TokenBase *open = pgm.nextToken();
    if ( !open || open->id() != TokenID::tkOpBrk )
	pgm.Throw(open) << "expecting ( after " << context << flush;

    TokenBase *first = pgm.nextToken();
    TokenBase *expr = pgm.parseExpression(first, true, false, stop_on_closing_paren, 1);
    if ( !expr )
	pgm.Throw(first ? first : open) << "Failed to parse " << context << " expression" << flush;

    return expr;
}

static int64_t parse_constant_primary(Program &pgm)
{
    TokenBase *tb = pgm.nextToken();
    int64_t out = 0;

    if ( resolve_integer_constant(pgm, tb, out) )
	return out;
    if ( tb && tb->id() == TokenID::tkNeg )
	return -parse_constant_primary(pgm);
    if ( tb && tb->id() == TokenID::tkOpBrk )
    {
	out = parse_constant_integer_expression(pgm);
	tb = pgm.nextToken();
	if ( !tb || tb->id() != TokenID::tkClBrk )
	    pgm.Throw(tb) << "Expecting ')' in constant expression" << flush;
	return out;
    }
    pgm.Throw(tb) << "Expecting integer constant expression" << flush;
    return 0;
}

static int64_t parse_constant_mul(Program &pgm)
{
    int64_t lhs = parse_constant_primary(pgm);

    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkMul && op->id() != TokenID::tkDiv && op->id() != TokenID::tkMod )
	    break;
	pgm.nextToken(); // consume operator
	int64_t rhs = parse_constant_primary(pgm);
	if ( op->id() == TokenID::tkMul ) lhs *= rhs;
	else if ( op->id() == TokenID::tkDiv )
	{
	    if ( rhs == 0 )
		pgm.Throw(op) << "Division by zero in constant expression" << flush;
	    lhs /= rhs;
	}
	else
	{
	    if ( rhs == 0 )
		pgm.Throw(op) << "Modulo by zero in constant expression" << flush;
	    lhs %= rhs;
	}
    }

    return lhs;
}

// additive: parse_constant_mul ([+-] parse_constant_mul)*
static int64_t parse_constant_add(Program &pgm)
{
    int64_t lhs = parse_constant_mul(pgm);

    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkAdd && op->id() != TokenID::tkSub && op->id() != TokenID::tkNeg )
	    break;
	pgm.nextToken(); // consume operator
	int64_t rhs = parse_constant_mul(pgm);
	if ( op->id() == TokenID::tkAdd ) lhs += rhs;
	else lhs -= rhs;
    }

    return lhs;
}

// shift: parse_constant_add ([<<>>] parse_constant_add)*
static int64_t parse_constant_shift(Program &pgm)
{
    int64_t lhs = parse_constant_add(pgm);

    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkBSL && op->id() != TokenID::tkBSR )
	    break;
	pgm.nextToken();
	int64_t rhs = parse_constant_add(pgm);
	if ( op->id() == TokenID::tkBSL ) lhs <<= rhs;
	else                              lhs >>= rhs;
    }

    return lhs;
}

// bitwise-and / xor / or: same precedence order as C.
static int64_t parse_constant_band(Program &pgm)
{
    int64_t lhs = parse_constant_shift(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkBand )
    {
	pgm.nextToken();
	lhs &= parse_constant_shift(pgm);
    }
    return lhs;
}

static int64_t parse_constant_bxor(Program &pgm)
{
    int64_t lhs = parse_constant_band(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkXor )
    {
	pgm.nextToken();
	lhs ^= parse_constant_band(pgm);
    }
    return lhs;
}

static int64_t parse_constant_bor(Program &pgm)
{
    int64_t lhs = parse_constant_bxor(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkBor )
    {
	pgm.nextToken();
	lhs |= parse_constant_bxor(pgm);
    }
    return lhs;
}

static int64_t parse_constant_integer_expression(Program &pgm)
{
    return parse_constant_bor(pgm);
}

DataDefVOID ddVOID;
DataDefVOIDref ddVOIDref;
DataDefBOOL ddBOOL;
DataDefCHAR ddCHAR;
DataDefINT ddINT;
DataDefINT8 ddINT8;
DataDefINT16 ddINT16;
DataDefINT24 ddINT24;
DataDefINT32 ddINT32;
DataDefINT64 ddINT64;
DataDefUINT8 ddUINT8;
DataDefUINT16 ddUINT16;
DataDefUINT24 ddUINT24;
DataDefUINT32 ddUINT32;
DataDefUINT64 ddUINT64;
DataDefFLOAT ddFLOAT;
DataDefDOUBLE ddDOUBLE;
DataDefSTRING ddSTRING;
DataDefSTRINGref ddSTRINGref;
DataDefISTREAM ddISTREAM;
DataDefOSTREAM ddOSTREAM;
DataDefSSTREAM ddSSTREAM;
DataDefARRAY ddARRAY;
DataDefIFSTREAM ddIFSTREAM;
DataDefOFSTREAM ddOFSTREAM;
DataDefFSTREAM ddFSTREAM;
DataDefLPSTR ddLPSTR;
DataDefPTR ddVOIDptr(ddVOID), ddCHARptr(ddCHAR), ddINTptr(ddINT), ddINT32ptr(ddINT32);
DataDefAUTO ddAUTO;
DataDefTEST ddTESTSTRUCT;


const char *c_str2(std::string *str)
{
    std::cout << "c_str2() on " << *str << '[' << (uint64_t)str << ']' << std::endl;
    std::cout << "c_str2() returning " << (uint64_t)str->c_str() << std::endl;
    uint64_t ui64 = (uint64_t)str->c_str();
    uint32_t ui32 = ui64;
    std::cout << "c_str2() uint32 " << ui32 << std::endl;
    return str->c_str();
}


void printuint32(uint32_t &i)
{
    std::cout << "i: " << i << std::endl << std::flush;
}

void printuint32(uint32_t i)
{
    std::cout << "i: " << i << std::endl << std::flush;
}

template<typename T> void streamout_type(std::ostream &os, T t)
{
    os << t;
}


void EatSpaces(istream &is)
{
    while ( is.good() && !is.eof() && isspace(is.peek()) )
        is.get();
}


int TokenAssign::ioperate() const
{
    DBG(std::cout << "TokenAssign" << std::endl);
    if ( left->type() != TokenType::ttVariable )
    {
	std::cerr << "TokenAssign::operate() left side not variable" << std::endl;
	return 0;
    }
    DBG(std::cout << "TokenAssign: " << dynamic_cast<TokenVar *>(left)->var.name << "=" << right->ival() << std::endl);
    left->set(right->ival());
    return right->ival();
}


// Variable constructor, will allocate data and initialize if requested
Variable::Variable(std::string n, DataDef &d, uint32_t c, void *init, bool alloc)
{
    name = n; 
    type = &d;
    count = c;
    flags = 0;
    data = NULL;
    if ( init ) { alloc = true; }
    if ( !alloc ) { flags |= vfSTACK; }
    switch(type->type())
    {
	case DataType::dtSTRING:
	    if ( init )
	    {
		data = new std::string((const char *)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new string for " << n << " (" << *(std::string *)data << ')' << std::endl);
	    }
	    else
	    if ( alloc )
	    {
		data = new std::string;
		flags |= vfALLOC;
	    }
	    DBG(std::cout << "Variable " << n << " Data address: " << (uint64_t)data << std::endl);
	    break;
	case DataType::dtSSTREAM:
	    if ( init )
	    {
		data = new std::stringstream((const std::string &)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new stringstream for " << n << " (" << *(std::string *)init << ')' << std::endl);
	    }
	    else
	    if ( alloc )
	    {
		data = new std::stringstream;
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new stringstream for " << n << std::endl);
	    }
	    DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
	    break;
	case DataType::dtISTREAM:
	    if ( init )
	    {
		data = new std::istream((streambuf *)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new istream for " << n << std::endl);
		DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
	    }
	    break;
	case DataType::dtOSTREAM:
	    if ( init )
	    {
		data = new std::ostream((streambuf *)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new ostream for " << n << std::endl);
		DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
	    }
	    break;
	default:
	    // Size 0 (e.g. FuncDef, void) has no storage. Function-pointer types
	    // (DataDefFPTR, size 8) are a pointer slot and DO need allocation.
	    if ( alloc && count == 1
	      && ((type->basetype() != BaseType::btFunct && type->size > 0)
	        || dynamic_cast<DataDefFPTR *>(type) != NULL) )
	    {
		data = calloc(count, d.size);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = calloc(" << count << ", " << d.size << ") for " << n << std::endl);
		DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
	    }
	    break;
    } // switch
}

Variable::~Variable()
{
    if ( !(flags & vfALLOC) )
	return;

    DBG(std::cout << "Variable::~Variable(" << name << ") freeing data" << std::endl);

    switch(type->type())
    {
	case DataType::dtSTRING:  delete (std::string *)data;		break;
	case DataType::dtSSTREAM: delete (std::stringstream *)data;	break;
	case DataType::dtOSTREAM: delete (std::ostream *)data;		break;
	default:		  free(data);				break;
    } // switch
}

Variable *DataDefCLASS::findMethod(std::string &s)
{
    // check method_map first (unmangled names)
    std::map<std::string, Variable *>::iterator it = method_map.find(s);
    if ( it != method_map.end() )
	return it->second;

    // fallback: search methods vector by variable name
    for ( variable_vec_iter vvi = methods.begin(); vvi != methods.end(); ++vvi )
	if ( !s.compare((*vvi)->name) )
	    return *vvi;

    return NULL;
}

DataDef *FuncDef::findParameter(std::string &s)
{
    DBG(cout << "FuncDef[" << name << "]::findParameter(" << s << ')' << endl);
    for ( datadef_vec_iter dvi = parameters.begin(); dvi != parameters.end(); ++dvi )
	if ( !s.compare((*dvi)->name) )
	    return *dvi;

    return NULL;
}

Variable *TokenCpnd::getParameter(unsigned int i)
{
    DBG(cout << "TokenCpnd::getParameter(" << i << ") method: " << (method ? method->returns.name : "NULL") << endl);
    return method ? method->getParameter(i) : NULL;
}

Variable *TokenCpnd::findParameter(std::string &id)
{
    DBG(cout << "TokenCpnd::findParameter(" << id << ") method: " << (method ? method->returns.name : "NULL") << endl);
    return method ? method->findParameter(id) : NULL;
}

// recursively search for local variables up the codeblock
Variable *TokenCpnd::findVariable(std::string &id)
{
    DBG(cout << "TokenCpnd::findVariable(" << id << ") method: " << (method ? method->returns.name : "NULL") << endl);
    for ( variable_vec_iter vvi = variables.begin(); vvi != variables.end(); ++vvi )
	if ( !id.compare((*vvi)->name) )
	    return *vvi;
    if ( parent )
	return parent->findVariable(id);

    return NULL;
}

Variable *Method::findVariable(std::string &s)
{
    for ( variable_vec_iter vvi = variables.begin(); vvi != variables.end(); ++vvi )
	if ( !s.compare((*vvi)->name) )
	    return *vvi;

    return NULL;
}

Variable *Method::findParameter(std::string &s)
{
    for ( variable_vec_iter vvi = parameters.begin(); vvi != parameters.end(); ++vvi )
	if ( !s.compare((*vvi)->name) )
	    return *vvi;

    return NULL;
}

typedef const char * (*fnSTRINGcstr)(void *);		// string::c_str()
typedef string & (*fnSTRINGmethodSTR)(const string &);	// string::append(string &)
typedef string & (*fnSTRINGmethodCSTR)(const char *);	// string::append(const char *)

union string_member_cast {
    const char * (string::*c_str)(void);
    string & (string::*method_str)(const string &);
    string & (string::*method_cstr)(const char *);
    void * void_pointer[1];
};

typedef string (*fnSSTREAMstr)(void *);		// stringstream::str()
union sstream_member_cast {
    string (stringstream::*str)(void) const;
    void * void_pointer[1];
};




// forward decl — body defined below next to other madc_string_* helpers
int64_t madc_string_length(void *str);

// add methods to ddSTRING
void Program::add_string_methods()
{
    string_member_cast scmc;
    Variable *var;

    scmc.c_str = (const char *(string::*)(void))&string::c_str;
    var = addFunction("c_str", datatype_vec_t{rtPtr(DataType::dtCHAR), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGcstr)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    var = addFunction("c_str2", datatype_vec_t{rtPtr(DataType::dtCHAR), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)c_str2, true);
    ddSTRING.methods.push_back(var);

    scmc.method_str = (string &(string::*)(const string &))&string::assign;
    var = addFunction("assign", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    scmc.method_cstr = (string &(string::*)(const char *))&string::assign;
    var = addFunction("assign", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodCSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    scmc.method_str = (string &(string::*)(const string &))&string::append;
    var = addFunction("append", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    scmc.method_cstr = (string &(string::*)(const char *))&string::append;
    var = addFunction("append", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodCSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    // length() and size() — wrap std::string::length via a free helper.
    // Signature: (int64_t, string*) matches madc method calling convention
    // where the object pointer is the hidden first argument.
    var = addFunction("length", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)madc_string_length, true);
    ddSTRING.methods.push_back(var);
    var = addFunction("size",   datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)madc_string_length, true);
    ddSTRING.methods.push_back(var);

    DBG(std::cout << "add_string_methods() ddSTRING.methods.size() = " << ddSTRING.methods.size() << std::endl);
}

// add methods to ddSSTREAM
void Program::add_sstream_methods()
{
    sstream_member_cast ssmc;
    Variable *var;

    ssmc.str = (string (stringstream::*)(void) const)&stringstream::str;
    var = addFunction("str", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSSTREAM)}, (fVOIDFUNC)(fnSSTREAMstr)ssmc.void_pointer[0], true);
    ddSSTREAM.methods.push_back(var);
}


// some debugging functions
void printinteger(int i)
{
    std::cout << i << std::endl;
}

void printuinteger(uint64_t i)
{
    std::cout << i << std::endl;
}

// some debugging functions
void printdouble(double d)
{
    std::cout << std::setprecision(16) << d << std::endl;
}

// some debugging functions
void printfloat(float f)
{
    std::cout << std::setprecision(8) << f << std::endl;
}

void printstarred(std::string &s)
{
    std::cout << "*** " << s << " ***" << std::endl;
}

void printstring(std::string *str)
{
    if ( !str ) { std::cerr << "ERROR: printstr: NULL!" << std::endl; return; }
    DBG(std::cout << "printstr(" << (uint64_t)str << "): " << *str << std::endl);
    cout << *str << endl;
}

void printstream(std::stringstream *os)
{
    if ( !os ) { std::cerr << "ERROR: printstream: NULL!" << std::endl; return; }
    DBG(std::cout << "printstream: " << os->str() << std::endl);
    cout << os->str() << endl;
}

// forward declarations for functions defined in compiler.cpp
extern void ifstream_open(void *, void *);
extern void ifstream_close(void *);
extern void ofstream_open(void *, void *);
extern void ofstream_close(void *);
extern void fstream_open(void *, void *);
extern void fstream_close(void *);
extern int64_t ifstream_eof(void *);
extern int64_t ifstream_good(void *);
extern int64_t ifstream_is_open(void *);
extern int64_t ofstream_good(void *);
extern int64_t ofstream_is_open(void *);
extern int64_t fstream_eof(void *);
extern int64_t fstream_good(void *);
extern int64_t fstream_is_open(void *);

// forward declarations for STL container methods (defined in ns_stl.cpp)
extern void vector_int_push_back(void *, int64_t);
extern void vector_int_pop_back(void *);
extern int64_t vector_int_at(void *, int64_t);
extern int64_t vector_int_size(void *);
extern void vector_int_clear(void *);
extern int64_t vector_int_empty(void *);
extern void vector_int_set(void *, int64_t, int64_t);
extern void vector_str_push_back(void *, void *);
extern void vector_str_pop_back(void *);
extern void *vector_str_at(void *, void *, int64_t);
extern void vector_str_set(void *, int64_t, void *);
extern int64_t vector_str_size(void *);
extern void vector_str_clear(void *);
extern int64_t vector_str_empty(void *);
extern void map_str_int_set(void *, void *, int64_t);
extern int64_t map_str_int_get(void *, void *);
extern int64_t map_str_int_contains(void *, void *);
extern void map_str_int_erase(void *, void *);
extern int64_t map_str_int_size(void *);
extern void map_str_int_clear(void *);
extern void map_str_str_set(void *, void *, void *);
extern void *map_str_str_get(void *, void *, void *);
extern int64_t map_str_str_contains(void *, void *);
extern int64_t map_str_str_size(void *);
extern void set_str_insert(void *, void *);
extern int64_t set_str_contains(void *, void *);
extern void set_str_erase(void *, void *);
extern int64_t set_str_size(void *);
extern void set_str_clear(void *);
extern void set_int_insert(void *, int64_t);
extern int64_t set_int_contains(void *, int64_t);
extern int64_t set_int_size(void *);
extern void list_int_push_back(void *, int64_t);
extern void list_int_push_front(void *, int64_t);
extern int64_t list_int_size(void *);
extern void list_int_clear(void *);
extern void list_str_push_back(void *, void *);
extern void list_str_push_front(void *, void *);
extern int64_t list_str_size(void *);

// dlopen/dlsym wrappers that accept std::string* (madc strings)
int64_t madc_dlopen(void *filename)
{
    const char *fn = ((std::string *)filename)->c_str();
    void *handle = dlopen(fn, RTLD_LAZY);
    if ( !handle )
	std::cerr << "dlopen: " << dlerror() << std::endl;
    return (int64_t)handle;
}

int64_t madc_dlsym(int64_t handle, void *name)
{
    const char *n = ((std::string *)name)->c_str();
    void *sym = dlsym((void *)handle, n);
    if ( !sym )
	std::cerr << "dlsym: " << dlerror() << std::endl;
    return (int64_t)sym;
}

void madc_dlclose(int64_t handle)
{
    if ( handle )
	dlclose((void *)handle);
}

// type conversion wrappers
void madc_to_string(void *result, int64_t val)
{
    *(std::string *)result = std::to_string(val);
}
void madc_to_string_d(void *result, double val)
{
    *(std::string *)result = std::to_string(val);
}
int64_t madc_stoi(void *str)
{
    try { return (int64_t)std::stoll(((std::string *)str)->c_str()); }
    catch (...) { return 0; }
}
double madc_stod(void *str)
{
    try { return std::stod(((std::string *)str)->c_str()); }
    catch (...) { return 0.0; }
}
int64_t madc_string_length(void *str)
{
    return (int64_t)((std::string *)str)->length();
}

// C library wrappers that accept madc strings
int64_t madc_system(void *cmd)
{
    return (int64_t)system(((std::string *)cmd)->c_str());
}

int64_t madc_getenv(void *result, void *name)
{
    const char *val = getenv(((std::string *)name)->c_str());
    std::string &res = *(std::string *)result;
    res = val ? val : "";
    return val ? 1 : 0;
}

const char *madc_get_argv(int64_t argv_ptr, int64_t index)
{
    char **argv = (char **)argv_ptr;
    return argv[index];
}

void madc_setenv(void *name, void *value)
{
    setenv(((std::string *)name)->c_str(), ((std::string *)value)->c_str(), 1);
}

void madc_unsetenv(void *name)
{
    unsetenv(((std::string *)name)->c_str());
}

// needed to add getline
typedef istream& (*fnGETLINE)(istream&, string&);
// needed to add endl
typedef ostream& (*fnENDL)(ostream&);

// add file stream methods
void Program::add_fstream_methods()
{
    Variable *var;

    // ifstream methods — must use typed wrappers (ios is virtual base, pointer offset differs)
    var = addFunction("open", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtIFSTREAM), DataType::dtSTRING}, (fVOIDFUNC)ifstream_open, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("close", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_close, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("eof", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_eof, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("good", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_good, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("is_open", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_is_open, true);
    ddIFSTREAM.methods.push_back(var);

    // ofstream methods
    var = addFunction("open", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtOFSTREAM), DataType::dtSTRING}, (fVOIDFUNC)ofstream_open, true);
    ddOFSTREAM.methods.push_back(var);
    var = addFunction("close", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtOFSTREAM)}, (fVOIDFUNC)ofstream_close, true);
    ddOFSTREAM.methods.push_back(var);
    var = addFunction("good", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtOFSTREAM)}, (fVOIDFUNC)ofstream_good, true);
    ddOFSTREAM.methods.push_back(var);

    // fstream methods
    var = addFunction("open", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtFSTREAM), DataType::dtSTRING}, (fVOIDFUNC)fstream_open, true);
    ddFSTREAM.methods.push_back(var);
    var = addFunction("close", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtFSTREAM)}, (fVOIDFUNC)fstream_close, true);
    ddFSTREAM.methods.push_back(var);
    var = addFunction("eof", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtFSTREAM)}, (fVOIDFUNC)fstream_eof, true);
    ddFSTREAM.methods.push_back(var);
    var = addFunction("good", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtFSTREAM)}, (fVOIDFUNC)fstream_good, true);
    ddFSTREAM.methods.push_back(var);
}

// add system library functions
void Program::add_functions()
{
    addFunction("printstarred", datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)printstarred );
    addFunction("printstr",     datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)printstring);
    addFunction("printstream",  datatype_vec_t{DataType::dtVOID, DataType::dtSSTREAM}, (fVOIDFUNC)printstream);
    addFunction("puts",		datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)puts);
    addFunction("puti",		datatype_vec_t{DataType::dtVOID, DataType::dtINT}, (fVOIDFUNC)printinteger);
    addFunction("putu",		datatype_vec_t{DataType::dtVOID, DataType::dtUINT64}, (fVOIDFUNC)printuinteger);
    addFunction("putd",		datatype_vec_t{DataType::dtVOID, DataType::dtDOUBLE}, (fVOIDFUNC)printdouble);
    addFunction("putf",		datatype_vec_t{DataType::dtVOID, DataType::dtFLOAT}, (fVOIDFUNC)printfloat);
    addFunction("putchar",	datatype_vec_t{DataType::dtINT,  DataType::dtINT}, (fVOIDFUNC)putchar);
    addFunction("getline",	datatype_vec_t{rtPtr(DataType::dtISTREAM),rtPtr(DataType::dtISTREAM),rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnGETLINE)std::getline);
    addFunction("endl",		datatype_vec_t{rtPtr(DataType::dtOSTREAM),rtPtr(DataType::dtOSTREAM)}, (fVOIDFUNC)(fnENDL)std::endl);
    // type conversion functions
    addFunction("to_string",	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)madc_to_string);
    addFunction("to_string_d",	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtDOUBLE}, (fVOIDFUNC)madc_to_string_d);
    addFunction("stoi",		datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_stoi);
    addFunction("stod",		datatype_vec_t{DataType::dtDOUBLE, DataType::dtSTRING}, (fVOIDFUNC)madc_stod);
    // strlen is NOT pre-registered: it resolves via dlsym fallback to libc's
    // strlen(const char *). For std::string, use str.length() or str.size().
    // C library functions
    addFunction("system",	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_system);
    addFunction("getenv",	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)madc_getenv);
    addFunction("get_argv",	datatype_vec_t{DataType::dtCHARptr, DataType::dtINT64, DataType::dtINT64}, (fVOIDFUNC)madc_get_argv);
    addFunction("setenv",	datatype_vec_t{DataType::dtVOID, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)madc_setenv);
    addFunction("unsetenv",	datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)madc_unsetenv);
    addFunction("__errno_location", datatype_vec_t{rtPtr(DataType::dtINT)}, (fVOIDFUNC)__errno_location);
    // dlopen/dlsym/dlclose — dynamic library loading
    addFunction("dlopen",	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_dlopen);
    addFunction("dlsym",	datatype_vec_t{DataType::dtINT64, DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_dlsym);
    addFunction("dlclose",	datatype_vec_t{DataType::dtVOID, DataType::dtINT64}, (fVOIDFUNC)madc_dlclose);
    // dlcall — call through function pointer (variadic, handled specially in compiler)
    addFunction("dlcall",	datatype_vec_t{DataType::dtINT64}, (fVOIDFUNC)NULL);
}

// define some global variables
void Program::add_globals()
{
    addGlobal(ddSTRING,  "version", 1, (void *)"v0.0.1");
}

enum { LAZY_IOSTREAM = 1, LAZY_STDIO = 2, LAZY_MATHH = 3 };

// populates lazy_map — symbols are registered on first use via lazy_resolve()
void Program::add_iostream()
{
    lazy_map["cout"] = {LAZY_IOSTREAM, Program::lkVariable};
    lazy_map["cin"]  = {LAZY_IOSTREAM, Program::lkVariable};
    lazy_map["cerr"] = {LAZY_IOSTREAM, Program::lkVariable};
}

void Program::add_stdio()
{
    // printf family available via dlsym fallback (libc is always loaded).
    // Register stdin/stdout/stderr for lazy resolution — each becomes an
    // int64 global whose data holds libc's current FILE* value.
    lazy_map["stdin"]  = {LAZY_STDIO, Program::lkVariable};
    lazy_map["stdout"] = {LAZY_STDIO, Program::lkVariable};
    lazy_map["stderr"] = {LAZY_STDIO, Program::lkVariable};
}

// on-demand variable/function registration — called from parseExpression()
Variable *Program::lazy_resolve(const std::string &name)
{
    std::map<std::string, LazyEntry>::iterator it = lazy_map.find(name);
    if ( it == lazy_map.end() )
	return NULL;
    if ( it->second.kind != lkVariable && it->second.kind != lkFunction )
	return NULL; // not a variable/function — leave for lazy_resolve_type

    Variable *var = NULL;
    int header = it->second.header;
    lazy_map.erase(it);

    if ( header == LAZY_IOSTREAM )
    {
	if ( name == "cout" )
	    var = addGlobal(ddOSTREAM, "cout", 1, std::cout.rdbuf());
	else if ( name == "cin" )
	    var = addGlobal(ddISTREAM, "cin", 1, std::cin.rdbuf());
	else if ( name == "cerr" )
	    var = addGlobal(ddOSTREAM, "cerr", 1, std::cerr.rdbuf());

	if ( var )
	    namespace_map["std"][name] = var;
    }
    else if ( header == LAZY_STDIO )
    {
	// dlsym the libc symbol and copy the current FILE* value into our
	// backing slot. Note: dlsym("stderr") returns the address of the
	// libc `FILE *stderr;` variable — one deref yields the FILE*.
	void **sym = NULL;
	if ( name == "stdin" || name == "stdout" || name == "stderr" )
	    sym = (void **)dlsym(RTLD_DEFAULT, name.c_str());
	if ( sym )
	{
	    var = addGlobal(ddINT64, name, 1, NULL);
	    if ( var && var->data )
		*(void **)var->data = *sym;
	}
    }

    DBG(if (var) std::cout << "lazy_resolve(" << name << ") registered" << std::endl);
    return var;
}

// on-demand type/struct registration — called from type lookup paths
DataDef *Program::lazy_resolve_type(const std::string &name)
{
    std::map<std::string, LazyEntry>::iterator it = lazy_map.find(name);
    if ( it == lazy_map.end() )
	return NULL;
    if ( it->second.kind != lkType && it->second.kind != lkStruct )
	return NULL;

    DataDef *dd = NULL;
    lazy_map.erase(it);

    // future: register struct layouts, typedefs from embedded headers
    // e.g. if ( header == LAZY_TIME_H && name == "time_t" ) dd = &ddINT64;

    DBG(if (dd) std::cout << "lazy_resolve_type(" << name << ") registered" << std::endl);
    return dd;
}

void Program::add_namespaces()
{
    Variable *var;
    std::string id;

    // std:: namespace — map to existing global variables and functions
    variable_map_t &std_ns = namespace_map["std"];

    // cout/cin/cerr/endl are registered by add_iostream() when #include <iostream> is processed

    // std::for_each(array, func_ptr) — iterate array calling function per element
    extern void std_for_each(void *, int64_t);
    var = addFunction("__std_for_each",
	datatype_vec_t{DataType::dtVOID, DataType::dtARRAY, DataType::dtINT64},
	(fVOIDFUNC)std_for_each);
    if (var) std_ns["for_each"] = var;

    DBG(std::cout << "add_namespaces() registered std:: with " << std_ns.size() << " members" << std::endl);
}

void Program::add_madc_namespace()
{
    variable_map_t &madc_ns = namespace_map["madc"];
    Variable *var;

    // register array type as madc::array
    std::string id = "__madc_array";
    var = new Variable(id, ddARRAY, 1, NULL, false);
    var->flags |= vfSTATIC;
    madc_ns["array"] = var;

    // regex functions
    extern int64_t madc_regex_match(void *, void *);
    extern int64_t madc_regex_search(void *, void *);
    extern void *madc_regex_replace(void *, void *, void *, void *);

    var = addFunction("__madc_regex_match",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_regex_match);
    if (var) madc_ns["regex_match"] = var;

    var = addFunction("__madc_regex_search",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_regex_search);
    if (var) madc_ns["regex_search"] = var;

    var = addFunction("__madc_regex_replace",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_regex_replace);
    if (var) madc_ns["regex_replace"] = var;

    DBG(std::cout << "add_madc_namespace() registered madc:: with " << madc_ns.size() << " members" << std::endl);
}

void Program::_parser_init()
{
    add_functions();
    add_string_methods();
    add_sstream_methods();
    add_fstream_methods();
    add_globals();
    // populate lazy_map for included headers (actual registration deferred to first use)
    if ( _include_iostream ) add_iostream();
    if ( _include_stdio )   add_stdio();
    add_namespaces();
    add_madc_namespace();
    add_php_namespace();
    add_perl_namespace();
    add_python_namespace();
    add_ruby_namespace();
    add_js_namespace();
    _braces = 0;
}

// find variable matching id anywhere accessable from codeblock
Variable *Program::findVariable(TokenCpnd *code, std::string &id)
{
    Variable *var;

    if ( code )
    {
	if ( (var=code->findVariable(id)) )
	    return var;
	if ( (var=code->findParameter(id)) )
	    return var;
    }
    if ( !(var=tkProgram->findVariable(id)) )
    {
	DBG(std::cout << "Program::findVariable(code, " << id << ") not found" << std::endl);
	return NULL;
    }

    DBG(std::cout << "Program::findVariable(code, " << id << ") found ptr: " << var << std::endl);

    return var;
}


Variable *Program::findVariable(std::string &s)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    variable_map_iter vmi;
    Variable *var;

    if ( code /*&& code->type() != TokenType::ttProgram*/ )
	return findVariable(code, s);

    if ( !(var=tkProgram->findVariable(s)) )
    {
	DBG(std::cout << "Program::findVariable(" << s << ") not found" << std::endl);
	return NULL;
    }

    DBG(std::cout << "Program::findVariable(" << s << ") found ptr: " << var << std::endl);

    return var;
}

// creates global variable named after string,
// duplicate strings will share same variable
Variable *Program::addLiteral(std::string &s)
{
    variable_map_iter vmi;
    Variable *var;

    string id = "__literal__";
    id.append(s);

    if ( (var=tkProgram->findVariable(id)) )
	return var;

    var = new Variable(id, ddSTRING, 1, NULL, true);
    var->makeconstant();
    std::string &str = *(std::string *)var->data;
    str = s;
    tkProgram->variables.push_back(var);

    return var;
}

Variable *Program::addVariable(TokenCpnd *code, DataDef &dd, std::string &id, int c, void *init, bool alloc)
{
    Variable *var;

    if ( code )
    {
	if ( (var=code->findVariable(id)) )
	    return var;
	var = new Variable(id, dd, c, init, alloc);
	code->variables.push_back(var);
	DBG(std::cout << "Added new variable type: " << dd.name << " size: "
		<< dd.size << " name: " << id << " ptr: " << var << " to codeblock: " << code << std::endl);
	DBG(std::cout << "Alloc: " << (alloc ? "true" : "false") << std::endl);
	DBG(std::cout << "Data address: " << (uint64_t)var->data << std::endl);
	return var;
    }
    if ( (var=tkProgram->findVariable(id)) )
    {
	if ( var->flags & vfEXTERN )
	{
	    var->type = &dd;
	    var->count = c;
	    if ( !parsing_extern_decl )
		var->flags &= ~vfEXTERN;
	}
	return var;
    }
    var = new Variable(id, dd, c, init, alloc);
    tkProgram->variables.push_back(var);

    DBG(std::cout << "Added new global variable type: " << dd.name << " size: "
		<< dd.size << " name: " << id << " ptr: " << var << " flags: " << var->flags << std::endl);
    DBG(std::cout << "Data address: " << (uint64_t)var->data << std::endl);

    return var;
}

// get or create a pointer-to-T DataDef
DataDefPTR *Program::getPointerType(DataDef *base)
{
    // check cache first
    auto it = ptr_type_cache.find(base);
    if ( it != ptr_type_cache.end() )
	return it->second;

    // return well-known globals for common types
    if ( base == &ddVOID )  return &ddVOIDptr;
    if ( base == &ddCHAR )  return &ddCHARptr;
    if ( base == &ddINT )   return &ddINTptr;
    if ( base == &ddINT32 ) return &ddINT32ptr;

    // create and cache a new pointer DataDef
    DataDefPTR *ptr = new DataDefPTR(*base);
    ptr_type_cache[base] = ptr;
    DBG(std::cout << "getPointerType() created " << ptr->name << " for base " << base->name << std::endl);
    return ptr;
}

// add a function definition
Variable *Program::addFunction(std::string id, datatype_vec_t params, fVOIDFUNC extfunc, bool isMethod)
{
    variable_map_iter vmi;
    funcdef_map_iter fmi;
    FuncDef *func;
    Variable *var;
    DataDef *dd;

    auto resolve_data_type = [this](DataType dt) -> DataDef *
    {
	if ( DataDef::rawtype(dt) != dt )
	{
	    if ( dt == rtPtr(DataType::dtCHAR) )
		return &ddLPSTR;

	    DataType raw = DataDef::rawtype(dt);
	    DataDef *base = NULL;
	    switch(raw)
	    {
		default:		   base = NULL;		break;
		case DataType::dtVOID:   base = &ddVOID;	break;
		case DataType::dtCHAR:   base = &ddCHAR;	break;
		case DataType::dtBOOL:   base = &ddBOOL;	break;
		case DataType::dtUINT8:  base = &ddUINT8;	break;
		case DataType::dtINT16:  base = &ddINT16;	break;
		case DataType::dtUINT16: base = &ddUINT16;	break;
		case DataType::dtINT32:  base = &ddINT32;	break;
		case DataType::dtUINT32: base = &ddUINT32;	break;
		case DataType::dtINT64:  base = &ddINT64;	break;
		case DataType::dtUINT64: base = &ddUINT64;	break;
		case DataType::dtSTRING: base = &ddSTRING;	break;
		case DataType::dtARRAY:  base = &ddARRAY;	break;
		case DataType::dtOSTREAM: base = &ddOSTREAM; break;
		case DataType::dtISTREAM: base = &ddISTREAM; break;
		case DataType::dtSSTREAM: base = &ddSSTREAM; break;
		case DataType::dtIFSTREAM: base = &ddIFSTREAM; break;
		case DataType::dtOFSTREAM: base = &ddOFSTREAM; break;
		case DataType::dtFSTREAM:  base = &ddFSTREAM;  break;
		case DataType::dtFLOAT:  base = &ddFLOAT;	break;
		case DataType::dtDOUBLE: base = &ddDOUBLE;	break;
	    }
	    if ( !base )
		return &ddVOID;
	    if ( static_cast<uint32_t>(dt) >= 20000 )
		return base;
	    return getPointerType(base);
	}

	switch(dt)
	{
	    default:	 	  return &ddVOID;
	    case DataType::dtCHAR:    return &ddCHAR;
	    case DataType::dtBOOL:    return &ddBOOL;
	    case DataType::dtUINT8:   return &ddUINT8;
	    case DataType::dtINT16:   return &ddINT16;
	    case DataType::dtUINT16:  return &ddUINT16;
	    case DataType::dtINT32:   return &ddINT32;
	    case DataType::dtUINT32:  return &ddUINT32;
	    case DataType::dtINT64:	  return &ddINT64;
	    case DataType::dtUINT64:  return &ddUINT64;
	    case DataType::dtSTRING:  return &ddSTRING;
	    case DataType::dtARRAY:   return &ddARRAY;
	    case DataType::dtOSTREAM: return &ddOSTREAM;
	    case DataType::dtISTREAM: return &ddISTREAM;
	    case DataType::dtSSTREAM: return &ddSSTREAM;
	    case DataType::dtIFSTREAM:return &ddIFSTREAM;
	    case DataType::dtOFSTREAM:return &ddOFSTREAM;
	    case DataType::dtFSTREAM: return &ddFSTREAM;
	    case DataType::dtFLOAT:   return &ddFLOAT;
	    case DataType::dtDOUBLE:  return &ddDOUBLE;
	}
    };

    // may have already been declared
    if ( !isMethod && (fmi=funcdef_map.find(id)) != funcdef_map.end() )
    {
	DBG(std::cout << "addFunction() already declared: " << id << std::endl);
	return NULL;
    }

    dd = resolve_data_type(params[0]);

    func = new FuncDef(*dd);
    if ( !isMethod )
	funcdef_map[id] = func;
    DBG(std::cout << "addFunction() Added new function declaration name: " << id << " numparams: " << params.size()-1  << " x86code: " << (uint64_t)extfunc << " returns " << dd->name << std::endl);

    // func->parameters.push_back(&pb->definition);

    for ( uint32_t i = 1; i < params.size(); ++i )
    {
	dd = resolve_data_type(params[i]);

	DBG(std::cout << dd->name);
	DBG(if (i < params.size()-1) std::cout << ", ");
	func->parameters.push_back(dd);
    }
    DBG(std::cout << endl);

    func->funcnode = NULL;
    Method *method;

    if ( isMethod )
    {
	var = new Variable(id, *func, 1, NULL, false);
	method = new Method(*var);
	var->data = (void *)method;
	method->x86code = (void *)extfunc;

	return var;
    }

    // check if this variable was already defined
    if ( (var=tkProgram->findVariable(id)) )
    {
	method = (Method *)var->data;
    }
    else
    {
	var = addVariable(NULL, *func, id, false);
	method = new Method(*var);
	var->data = (void *)method;
    }
    method->x86code = (void *)extfunc;

    return var;
}


void Program::pushCompound()
{
    TokenCpnd *tc = new TokenCpnd;

    ++_braces;

    if ( compounds.empty() )
    {
	DBG(std::cout << "pushCompound(" << _braces << ") function" << std::endl);
	tc->parent = NULL;
    }
    else
    {
	DBG(std::cout << "pushCompound(" << _braces << ") nested" << std::endl);
	tc->parent = compounds.top();
	tc->method = compounds.top()->method;
	compounds.top()->child = tc;
    }
    compounds.push(tc);
}

void Program::popCompound()
{
    DBG(std::cout << "popCompound(" << _braces << ')' << std::endl);
    --_braces;
    if ( !compounds.empty() )
	compounds.pop();
}

void Program::popOperator(stack<TokenBase *> &opStack, stack<TokenBase *> &exStack)
{
    DBG(cout << "popOperator() size: " << opStack.size() << " TOP" << endl);
    TokenOperator *to;

    switch(opStack.top()->type())
    {
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	    to = (TokenOperator *)opStack.top();
	    if ( to->type() == TokenType::ttOperator )
		DBG(cout << "popOperator() got operator: " << (char)to->get() << " id() " << (int)to->id() << endl);
	    else
		DBG(cout << "popOperator() got operator: " << ((TokenMultiOp *)to)->str << endl);
	    if ( to->argc() > 0 )
	    {
		if ( !to->right )
		{
		    if ( exStack.empty() )
		    {
			DBG(cerr << "got operator, but exStack is empty!" << endl);
			Throw(to) << "Missing operand" << flush;
		    }
		    to->right = exStack.top(); exStack.pop(); DBG(cout << "popped " << to->right->ival() << endl);
		}
		if ( to->argc() > 1 )
		{
		    if ( !to->left )
		    {
			if ( exStack.empty() )
			{
			    DBG(cerr << "got operator, but exStack is empty!" << endl);
			    Throw(to) << "Missing operand" << flush;
			}
			to->left = exStack.top(); exStack.pop();  DBG(cout << "popped " << to->left->ival() << endl);
		    }
		}
	    }
	    DBG(cout << "Popping " << (char)to->get() << "[" << (to->left ? to->left->ival() : 0) << ", " << (to->right ? to->right->ival() : 0) << "] from opStack and onto exStack" << endl);
	    opStack.pop();
	    exStack.push(to);
	    break;
	case TokenType::ttCallFunc:
	    DBG(cout << "popOperator() got ttCallFunc" << endl);
	    exStack.push(opStack.top());
	    opStack.pop();
	    break;
	case TokenType::ttCallMethod:
	    DBG(cout << "popOperator() got ttCallMethod" << endl);
	    exStack.push(opStack.top());
	    opStack.pop();
	    break;
	default:
	    DBG(cerr << "popOperator() throwing opStack.top()" << endl);
	    Throw(opStack.top()) << "unexpected token type " << (int)opStack.top()->type() << flush;
    } // end switch
    DBG(cout << "popOperator() size: " << opStack.size() << " END" << endl);
}

#if 0
// parse a function call and it's parameters
// parameters are individually parsed by parseExpression
// returns ending token
TokenBase *Program::parseCallFunc(TokenCallFunc *tc)
{
    TokenBase *tb;

    DBG(std::cout << tc->line << ':' << tc->column << ":Program::parseCallFunc(" << tc->var.name << ')' << std::endl);
    int brackets = 1;
    size_t paramcnt = 0;

    if ( !(tb=peekToken()) )
	throw "Unexpected end of input";
    while ( brackets && tb->id() != TokenID::tkSemi )
    {
	tb = nextToken();
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	if ( tb->id() == TokenID::tkComma )
	{
	    if ( ++paramcnt >= ((FuncDef *)tc->var.type)->parameters.size() )
		throw "Too many parameters";
	    continue;
	}
	if ( tb->id() == TokenID::tkSemi ) { DBG(cout << "Got ;" << endl); break; }
	DBG(cout << "parseCallFunc() brackets: " << brackets << " tokenID(" << (char)tb->get() << "): " << (int)tb->id() << " calling parseExpression" << endl);
	if ( !(tb=parseExpression(tb, true)) ) { DBG(cout << "parseExp return NULL" << endl); break; }
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	DBG(cout << "parseExpression returned type(): " << (int)tb->type() << " id(): " << (int)tb->id() << endl);
	DBG(cout << "calling tc(" << tc->var.name << ")[" << (uint64_t)tc << "]->parameters.push_back(tb[" << (uint64_t)tb << "]) brackets: " << brackets << endl);
	tc->parameters.push_back(tb);
    }
    if ( tb->id() == TokenID::tkSemi )
	DBG(cout << "parseCallFunc() while ended on semicolon" << endl);
    // (need check for optional parameters)
    if ( tc->argc() != ((FuncDef *)tc->var.type)->parameters.size() )
    {
	DBG(std::cout << "parseCallFunc(" << tc->var.name << "): argument count: " << tc->argc() << " expected: " << ((FuncDef *)tc->var.type)->parameters.size() << " (paramcnt: " << paramcnt << ") brackets: " << brackets << std::endl);
	throw "Incorrect number of parameters";
    }

    return tb;
}
#else
static bool call_accepts_extra_args(TokenCallFunc *tc)
{
    FuncDef *fd = (FuncDef *)tc->var.type;
    Method *md = (Method *)tc->var.data;

    if ( fd->is_varargs )
	return true;
    if ( fd->parameters.empty() && md && (md->x86code || tc->var.name == "dlcall") )
	return true;
    return false;
}

// parse a function call and it's parameters
// parameters are individually parsed by parseExpression
// returns ending token
TokenBase *Program::parseCallFunc(TokenCallFunc *tc)
{
    TokenBase *tb;

    DBG(std::cout << tc->line << ':' << tc->column << ":Program::parseCallFunc(" << tc->var.name << ')' << std::endl);
#if 0
    tb = nextToken();
    if ( tb->id() != TokenID::tkOpBrk )
    {
	DBG(std::cout << "Program::parseCallFunc() no parameters" << std::endl);
	return tb;
    }
#endif
    int brackets = 1;
    size_t paramcnt = 0;

    while ( brackets )
    {
	tb = peekToken();
	if ( tb->id() == TokenID::tkSemi )  { return tb; }
	tb = nextToken();
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	if ( tb->id() == TokenID::tkComma )
	{
	    FuncDef *_fd = (FuncDef *)tc->var.type;
	    if ( !_fd->parameters.empty()
	    &&   !call_accepts_extra_args(tc)
	    &&   ++paramcnt >= _fd->parameters.size() )
		Throw(tb) << "Too many parameters" << flush;
	    continue;
	}
	if ( tb->id() == TokenID::tkSemi ) { break; }
	DBG(cout << "parseCallFunc() brackets: " << brackets << " tokenID(" << (char)tb->get() << "): " << (int)tb->id() << " calling parseExpression" << endl);
	if ( !(tb=parseExpression(tb, true)) ) { break; }
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	DBG(cout << "parseExpression returned type(): " << (int)tb->type() << " id(): " << (int)tb->id() << endl);
	DBG(cout << "calling tc(" << tc->var.name << ")[" << (uint64_t)tc << "]->parameters.push_back(tb[" << (uint64_t)tb << "])" << endl);
	tc->parameters.push_back(tb);
    }
    // (need check for optional parameters)
    // skip arg count check for dlopen functions (0 declared params = variadic-like)
    {
	// function pointer variable: type is DataDefFPTR, get target FuncDef
	if ( tc->var.type->is_function() && tc->var.type->is_numeric() )
	{
	    DataDefFPTR *fptr = static_cast<DataDefFPTR *>(tc->var.type);
	    FuncDef *fd = fptr->target;
	    // don't count hidden env param for [&] lambdas
	    size_t expected = fd->parameters.size() - (fd->has_captures ? 1 : 0);
	    if ( tc->argc() != expected )
		Throw(tc) << "Incorrect number of parameters: expected " << expected << " got " << tc->argc() << flush;
	}
	else
	{
	    FuncDef *fd = (FuncDef *)tc->var.type;
	    Method *md = (Method *)tc->var.data;
	    if ( !(fd->parameters.empty() && md && (md->x86code || tc->var.name == "dlcall")) )
	    {
		size_t expected = fd->parameters.size() - (fd->has_captures ? 1 : 0)
			- (md && md->owner_class ? 1 : 0)
			- (fd->is_varargs ? 1 : 0);
		// varargs functions accept expected or more args; fixed functions require exact match
		if ( fd->is_varargs ? (tc->argc() < expected) : (tc->argc() != expected) )
		{
		    DBG(std::cout << "parseCallFunc: argument count: " << tc->argc() << " expected: " << expected << std::endl);
		    Throw(tc) << "Incorrect number of parameters: expected " << expected << " got " << tc->argc() << flush;
		}
	    }
	}
    }

    return tb;
}
#endif

// parse a method call and it's parameters
// parameters are individually parsed by parseExpression
// returns ending token
TokenBase *Program::parseCallMethod(TokenCallMethod *tc)
{
    TokenBase *tb;

    DBG(std::cout << tc->line << ':' << tc->column << ":Program::parseCallMethod(" << tc->var.name << ')' << std::endl);
    int brackets = 1;
    size_t paramcnt = 1;

    while ( brackets )
    {
	tb = peekToken();
	if ( tb->id() == TokenID::tkSemi )  { return tb; }
	tb = nextToken();
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	if ( tb->id() == TokenID::tkComma )
	{
	    if ( !((FuncDef *)tc->var.type)->is_varargs
	    &&   ++paramcnt >= ((FuncDef *)tc->var.type)->parameters.size() )
		Throw(tb) << "Too many parameters" << flush;
	    continue;
	}
	if ( tb->id() == TokenID::tkSemi ) { break; }
	DBG(cout << "parseCallMethod() brackets: " << brackets << " tokenID(" << (char)tb->get() << "): " << (int)tb->id() << " calling parseExpression" << endl);
	if ( !(tb=parseExpression(tb, true)) ) { break; }
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	DBG(cout << "parseExpression returned type(): " << (int)tb->type() << " id(): " << (int)tb->id() << endl);
	DBG(cout << "calling tc(" << tc->var.name << ")[" << (uint64_t)tc << "]->parameters.push_back(tb[" << (uint64_t)tb << "])" << endl);
	tc->parameters.push_back(tb);
    }
    // (need check for optional parameters)
    if ( ((FuncDef *)tc->var.type)->is_varargs
      ? (tc->argc()+1 < ((FuncDef *)tc->var.type)->parameters.size()-1)
      : (tc->argc()+1 != ((FuncDef *)tc->var.type)->parameters.size()) )
    {
	DBG(std::cout << "parseCallMethod: argument count: " << tc->argc() << " expected: " << ((FuncDef *)tc->var.type)->parameters.size() << std::endl);
	Throw(tc) << "Incorrect number of parameters: expected " << ((FuncDef *)tc->var.type)->parameters.size() << " got " << tc->argc()+1 << flush;
    }

    return tb;
}


// parse one complete expression
// for expression: x = 5, sum(5, 5), ++x, etc
// a "conditional" expression stops when brackets are equalized
// Parse an identifier followed by any chain of postfix operators
// (->ident, .ident, [expr]) and return the resulting node. Stops at the
// first non-postfix token (binary operator, comma, semicolon, etc.);
// that token remains consumable by the caller on the next nextToken().
// Used by unary `*` to avoid parseExpression's greedy consumption of
// trailing binary operators such as `== '$'`.
TokenBase *Program::parsePostfixChain(TokenBase *head)
{
    if ( !head || head->type() != TokenType::ttIdentifier )
	return NULL;

    std::string name = ((TokenIdent *)head)->str;
    Variable *var = findVariable(name);
    if ( !var )
	Throw(head) << "undeclared identifier '" << name << "'" << flush;

    TokenBase *result = new TokenVar(*var);
    result->file = head->file;
    result->line = head->line;
    result->column = head->column;

    while ( peekToken() )
    {
	TokenID pid = peekToken()->id();
	if ( pid == TokenID::tkDeRef || pid == TokenID::tkDot )
	{
	    bool is_arrow = (pid == TokenID::tkDeRef);
	    TokenBase *op_tb = nextToken();
	    TokenBase *mtb = nextToken();
	    if ( !mtb || !is_contextual_identifier_token(mtb) )
		Throw(mtb ? mtb : op_tb) << "expected member name after '"
		    << (is_arrow ? "->" : ".") << "'" << flush;
	    std::string mname = contextual_identifier_name(mtb);

	    DataDef *obj_type = result->datadef();
	    if ( is_arrow )
	    {
		if ( !obj_type || !obj_type->is_pointer() )
		    Throw(mtb) << "expression before '->' must be a pointer" << flush;
		DataDefPTR *pt = dynamic_cast<DataDefPTR *>(obj_type);
		if ( !pt || !pt->base_type )
		    Throw(mtb) << "expression before '->' is not a typed pointer" << flush;
		obj_type = pt->base_type;
	    }
	    if ( !obj_type || (!obj_type->is_struct() && !obj_type->is_object()) )
		Throw(mtb) << "member reference type is not a structure or union" << flush;
	    DataDefSTRUCT *sdd = static_cast<DataDefSTRUCT *>(obj_type);
	    ssize_t ofs = sdd->m_offset(mname);
	    if ( ofs == -1 )
		Throw(mtb) << "no member named '" << mname << "'" << flush;
	    DataDef *mtype = sdd->m_type(mname);
	    Variable *mvar = new Variable(mname, *mtype, 1, NULL, false);
	    mvar->flags = var->flags;

	    TokenMember *tm;
	    if ( result->type() == TokenType::ttVariable )
	    {
		TokenVar *tv = dynamic_cast<TokenVar *>(result);
		tm = new TokenMember(tv->var, *mvar, ofs);
	    }
	    else
	    {
		tm = new TokenMember(*var, *mvar, ofs, result);
	    }
	    tm->file = op_tb->file;
	    tm->line = op_tb->line;
	    tm->column = op_tb->column;
	    result = tm;
	    continue;
	}
	if ( pid == TokenID::tkOpSqr )
	{
	    TokenBase *open = nextToken();
	    TokenBase *idx_tb = nextToken();
	    TokenBase *idx_expr = parseExpression(idx_tb, true);
	    TokenBase *close = nextToken();
	    if ( !close || close->id() != TokenID::tkClSqr )
		Throw(close ? close : open) << "expected ']' in subscript" << flush;
	    DataDef *base_type = result->datadef();
	    DataDef *elem_type = &ddINT64;
	    // Fixed-array subscripts preserve the element type directly (the
	    // `struct node *` in `struct node *arr[2]` is already the element),
	    // whereas raw-pointer subscripts dereference one level.
	    bool fixed_array = false;
	    if ( TokenVar *tv = dynamic_cast<TokenVar *>(result) )
		fixed_array = tv->var.is_fixed_array();
	    if ( fixed_array )
		elem_type = base_type ? base_type : &ddINT64;
	    else if ( base_type && base_type->is_pointer() )
	    {
		DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(base_type);
		elem_type = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
	    }
	    result = new TokenSubscriptExpr(result, idx_expr, elem_type);
	    continue;
	}
	break;
    }
    return result;
}

TokenBase *Program::parseExpression(TokenBase *tb, bool conditional, bool ternary_branch,
				    bool stop_on_closing_paren, int initial_brackets)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenOperator *to;
    stack<TokenBase *> exStack;
    stack<TokenBase *> opStack;
    TokenDataType *bt;
    Variable *var;
    bool done = false;
    int brackets = initial_brackets;

    DBG(std::cout << tb->line << ':' << tb->column << ":Program::parseExpression(" << tb->get() << " type: " << (int)tb->type() << ") start" << (conditional ? " conditional" : "") << std::endl);

    while ( !done && tb )
    {
	switch(tb->type())
	{
	    case TokenType::ttInteger:
	        DBG(cout << "Pushing integer: " << (int)tb->get() << " onto exStack" << endl);
		exStack.push(tb); // exStack.push(tb->clone());
		break;
	    case TokenType::ttReal:
	        DBG(cout << "Pushing number: " << ((TokenReal *)tb)->dval() << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    case TokenType::ttSymbol:
	    	if ( tb->id() == TokenID::tkSemi )
		{
		    DBG(cout << "parseExpression: found semicolon" << endl);
		    done = true;
		}
	    	if ( tb->id() == TokenID::tkComma )
		{
		    DBG(cout << "parseExpression: found comma" << endl);
		    done = true;
		}
		break;
	    case TokenType::ttMultiOp:
	    case TokenType::ttOperator:
	    	if ( tb->id() == TokenID::tkComma )
		{
		    DBG(cout << "parseExpression: found comma" << endl);
		    done = true;
		    break;
		}
		// subscript: var[index] or lambda: [](params) { body }
		if ( tb->id() == TokenID::tkOpSqr )
		{
		    // Chained subscript on a multi-dim C fixed-size array: arr[i][j]
		    // Append to the existing TokenSubscript's extra_indices vector.
		    if ( !exStack.empty() && exStack.top()->type() == TokenType::ttSubscript )
		    {
			TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(exStack.top());
			if ( tsub && tsub->object.is_fixed_array()
			  && tsub->extra_indices.size() + 2 <= tsub->object.dims.size() )
			{
			    TokenBase *idx = parseExpression(nextToken());
			    TokenBase *clsqr = nextToken(); // consume ]
			    if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
				Throw(tb) << "Expected ] in subscript expression" << flush;
			    tsub->extra_indices.push_back(idx);
			    break;
			}
		    }
		    // if top of exStack is a variable, treat [ as subscript operator
		    if ( !exStack.empty() && exStack.top()->type() == TokenType::ttVariable )
		    {
			static int sub_tmp_counter = 0;
			TokenVar *tv = dynamic_cast<TokenVar *>(exStack.top());
			exStack.pop();
			// parse index expression (stops at ] via peek-stop below)
			TokenBase *idx = parseExpression(nextToken());
			TokenBase *clsqr = nextToken(); // consume ]
			if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
			    Throw(tb) << "Expected ] in subscript expression" << flush;
			// for string-returning containers, allocate a temp variable for the result
			Variable *tmp = nullptr;
			if ( tv->var.type->type() == DataType::dtVECTOR ) {
			    DataDefVECTOR *vdd = static_cast<DataDefVECTOR *>(tv->var.type);
			    if ( vdd->element_type->is_string() ) {
				std::string tmpname = "__sub_tmp_" + std::to_string(sub_tmp_counter++);
				tmp = addVariable(code, ddSTRING, tmpname, 1, NULL, true);
			    }
			} else if ( tv->var.type->type() == DataType::dtMAP ) {
			    DataDefMAP *mdd = static_cast<DataDefMAP *>(tv->var.type);
			    if ( mdd->val_type->is_string() ) {
				std::string tmpname = "__sub_tmp_" + std::to_string(sub_tmp_counter++);
				tmp = addVariable(code, ddSTRING, tmpname, 1, NULL, true);
			    }
			}
			DBG(cout << "parseExpression: subscript on " << tv->var.name << endl);
			exStack.push(new TokenSubscript(tv->var, idx, tmp));
			break;
		    }
		    // Widened detection: complex value-producing expressions
		    // whose datadef() reports a pointer (TokenAdd / TokenSub /
		    // TokenAssign — see their datadef() overrides). Lets
		    // `(p + n)[i]`, `(q = p + 1)[i]`, `(mud = imc_mudof(arg))[0]`
		    // parse as subscript-on-pointer rather than lambda.
		    // TokenSubscriptExpr::compile branches on the base_expr type
		    // and uses compile() (not operand()) for these complex bases.
		    bool top_is_complex_ptr_expr = false;
		    if ( !exStack.empty() )
		    {
			TokenBase *xt = exStack.top();
			if ( dynamic_cast<TokenAdd *>(xt) != NULL
			  || dynamic_cast<TokenSub *>(xt) != NULL
			  || dynamic_cast<TokenAssign *>(xt) != NULL )
			{
			    // Direct datadef() check first — covers
			    // `(char *p + n)[i]` where p is a real pointer.
			    DataDef *xd = xt->datadef();
			    if ( xd && xd->is_pointer() )
				top_is_complex_ptr_expr = true;
			    // Fallback: a TokenVar of a fixed-array decays
			    // to a pointer in arithmetic context, but its
			    // raw datadef reports the element type. Walk
			    // into TokenAdd/Sub operands to find such a
			    // base. Surfaced by `(buf + strlen(buf))[i]`
			    // where buf is `char[N]`.
			    if ( !top_is_complex_ptr_expr )
			    {
				TokenBase *op_l = NULL, *op_r = NULL;
				if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(xt) )
				    { op_l = ta->left; op_r = ta->right; }
				else if ( TokenSub *ts = dynamic_cast<TokenSub *>(xt) )
				    { op_l = ts->left; op_r = ts->right; }
				else if ( TokenAssign *tas = dynamic_cast<TokenAssign *>(xt) )
				    { op_l = tas->left; op_r = tas->right; }
				auto is_fixed_array_var = [](TokenBase *t) -> bool {
				    if ( !t ) return false;
				    if ( TokenVar *tv = dynamic_cast<TokenVar *>(t) )
					return tv->var.is_fixed_array();
				    return false;
				};
				if ( is_fixed_array_var(op_l) || is_fixed_array_var(op_r) )
				    top_is_complex_ptr_expr = true;
			    }
			}
		    }
		    if ( !exStack.empty()
		      && (exStack.top()->type() == TokenType::ttMember
		       || exStack.top()->type() == TokenType::ttSubscript
		       || dynamic_cast<TokenDerefExpr *>(exStack.top()) != NULL
		       || dynamic_cast<TokenDeref *>(exStack.top()) != NULL
		       || top_is_complex_ptr_expr) )
		    {
			TokenBase *base_expr = exStack.top();
			exStack.pop();
			TokenBase *idx = parseExpression(nextToken());
			TokenBase *clsqr = nextToken(); // consume ]
			if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
			    Throw(tb) << "Expected ] in subscript expression" << flush;
			DataDef *elem_type = base_expr->datadef();
			// Distinguish a fixed-array struct member (e.g.
			// `SKILLTYPE *arr[N]` — array of pointers) from a
			// stored pointer member (`SKILLTYPE *p`). For the
			// first, `arr[i]` yields the declared element type
			// as-is (still a SKILLTYPE *); for the second, `p[i]`
			// derefs the pointer to its base type.
			TokenMember *tm = dynamic_cast<TokenMember *>(base_expr);
			bool member_is_fixed_array = tm && tm->is_fixed_array_member();
			if ( !member_is_fixed_array && elem_type && elem_type->is_pointer() )
			{
			    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(elem_type);
			    elem_type = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
			}
			// Fixed-array decay: when the base was widened via the
			// fixed-array fallback, the chain's datadef() reports the
			// element type (TokenVar of fixed-array does so), and
			// is_pointer is false — so the unwrap above didn't fire.
			// Walk into TokenAdd/Sub/Assign operands to recover the
			// fixed-array's element type. Surfaced by `(buf + N)[i]`
			// where buf is `char[K]`.
			if ( top_is_complex_ptr_expr
			  && (!base_expr->datadef() || !base_expr->datadef()->is_pointer()) )
			{
			    TokenBase *op_l = NULL, *op_r = NULL;
			    if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(base_expr) )
				{ op_l = ta->left; op_r = ta->right; }
			    else if ( TokenSub *ts = dynamic_cast<TokenSub *>(base_expr) )
				{ op_l = ts->left; op_r = ts->right; }
			    else if ( TokenAssign *tas = dynamic_cast<TokenAssign *>(base_expr) )
				{ op_l = tas->left; op_r = tas->right; }
			    auto fixed_array_var = [](TokenBase *t) -> Variable * {
				if ( !t ) return NULL;
				if ( TokenVar *tv = dynamic_cast<TokenVar *>(t) )
				    return tv->var.is_fixed_array() ? &tv->var : NULL;
				return NULL;
			    };
			    Variable *fav = fixed_array_var(op_l);
			    if ( !fav ) fav = fixed_array_var(op_r);
			    if ( fav )
				elem_type = fav->type;
			}
			DBG(cout << "parseExpression: subscript on expression base" << endl);
			exStack.push(new TokenSubscriptExpr(base_expr, idx, elem_type));
			break;
		    }
		    DBG(cout << "parseExpression: detected [ — parsing lambda" << endl);
		    TokenBase *lambda = parseLambda();
		    exStack.push(lambda);
		    break;
		}
		if ( tb->id() == TokenID::tkOpBrk )
		{
		    // check for cast expression: (TYPE [*...]) expr
		    TokenBase *peek1 = peekToken();
		    DataDef *cast_dd = NULL;
		    if ( peek1 )
		    {
			if ( peek1->type() == TokenType::ttDataType )
			    cast_dd = &((TokenDataType *)peek1)->definition;
			else if ( peek1->id() == TokenID::tkSTRUCT )
			{
			    // (struct Tag *) — peek further
			    TokenBase *save1 = nextToken(); // consume 'struct'
			    TokenBase *save2 = peekToken();
			    if ( save2 && save2->type() == TokenType::ttIdentifier )
			    {
				std::string sname = ((TokenIdent *)save2)->str;
				datadef_map_iter sdmi = struct_map.find(sname);
				if ( sdmi != struct_map.end() )
				{
				    nextToken(); // consume tag name
				    cast_dd = sdmi->second;
				}
				else
				{
				    pushToken(save1); // push 'struct' back
				}
			    }
			    else
				pushToken(save1);
			}
			else if ( peek1->type() == TokenType::ttIdentifier )
			{
			    std::string tname = ((TokenIdent *)peek1)->str;
			    datatype_map_iter tdmi = datatype_map.find(tname);
			    if ( tdmi != datatype_map.end() )
				cast_dd = &tdmi->second->definition;
			}
		    }
		    if ( cast_dd )
		    {
			// speculatively consume the type token (if not struct, which was already consumed)
			if ( peekToken() && (peekToken()->type() == TokenType::ttDataType
			||  (peekToken()->type() == TokenType::ttIdentifier && datatype_map.count(((TokenIdent *)peekToken())->str))) )
			    nextToken();
			// consume pointer stars
			while ( peekToken() && peekToken()->id() == TokenID::tkMul )
			{
			    nextToken();
			    cast_dd = getPointerType(cast_dd);
			}
			// Function-pointer cast: `(RET (*)(PARAMS)) expr`. After the
			// return type (plus any pointer stars) we may see `(*)` and
			// then a parameter list. Reuse parseFnPtrParams() to build the
			// FuncDef, then wrap in DataDefFPTR.
			if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
			{
			    TokenBase *open = nextToken();    // consume '('
			    TokenBase *star = peekToken();
			    if ( star && star->id() == TokenID::tkMul )
			    {
				nextToken(); // consume '*'
				TokenBase *close1 = nextToken();
				if ( !close1 || close1->id() != TokenID::tkClBrk )
				    Throw(close1 ? close1 : open) << "expected ')' after '(*' in function pointer cast" << flush;
				TokenBase *open2 = nextToken();
				if ( !open2 || open2->id() != TokenID::tkOpBrk )
				    Throw(open2 ? open2 : open) << "expected '(' to introduce parameter list in function pointer cast" << flush;
				FuncDef *func = parseFnPtrParams(*cast_dd);
				cast_dd = new DataDefFPTR(func);
			    }
			    else
			    {
				// not a function-pointer cast — push '(' back and fall
				// through to the regular close-paren handling below.
				pushToken(open);
			    }
			}
			// must have closing )
			if ( peekToken() && peekToken()->id() == TokenID::tkClBrk )
			{
			    nextToken(); // consume )
			    TokenBase *cast_expr_tb = nextToken();
			    // Null out _prv_token so unary operators at the head of
			    // the cast body (`&addr`, `*ptr`, `-x`) see a unary
			    // position. Otherwise the cast's close-paren leaks into
			    // isUnaryPosition and they mis-parse as binary ops.
			    _prv_token = NULL;
			    TokenBase *cast_expr = NULL;
			    // Casts in C bind tighter than binary operators: `(long)q
			    // - n` means `((long)q) - n`, not `(long)(q - n)`. When
			    // the body is a bare identifier (with an optional postfix
			    // chain of ->/./[] accesses), use parsePostfixChain which
			    // stops at the first non-postfix token. Function calls
			    // (`ident(args)`) and everything else (parenthesized
			    // body, unary-operator head) still go through
			    // parseExpression so the full call or complex expression
			    // parses correctly.
			    bool ident_no_call = cast_expr_tb
			      && cast_expr_tb->type() == TokenType::ttIdentifier
			      && !(peekToken() && peekToken()->id() == TokenID::tkOpBrk);
			    if ( ident_no_call )
			    {
				cast_expr = parsePostfixChain(cast_expr_tb);
			    }
			    else if ( cast_expr_tb && cast_expr_tb->id() == TokenID::tkOpBrk )
			    {
				// Cast body is a parenthesized primary, e.g.
				// `(int)(a - b)` inside `cout << (int)(a-b) << endl`.
				// The cast binds to that primary only — anything
				// after the matching ')' (a binary `<<`, etc.) belongs
				// to the enclosing expression, not the cast body.
				// Consume the '(' and parse with stop_on_closing_paren
				// so parseExpression returns after the matching ')'.
				// (postfix follow-ups like `(MyType*)(p+1)->m` keep
				// parsing because postfix `->` binds tighter than the
				// cast — that path is already handled inside
				// parseExpression's close-paren branch.)
				TokenBase *first_inner = nextToken();
				cast_expr = parseExpression(first_inner, true, false, true, 1);
			    }
			    else
			    {
				cast_expr = parseExpression(cast_expr_tb, true);
			    }
			    exStack.push(new TokenCast(cast_dd, cast_expr));
			    DBG(cout << "parseExpression: cast to " << cast_dd->name << endl);
			    break;
			}
			// not a cast after all — fall through to grouping
			// (this shouldn't happen in practice for valid C code)
		    }
		    // Direct invocation through a struct-member function pointer,
		    // e.g. `cmd.fn(3, 4)` or `tab[i].fn(ch, arg)`. Detected when the
		    // top of exStack is a TokenMember whose datadef is DataDefFPTR.
		    // The `(` must IMMEDIATELY follow the member access — if any
		    // tighter-than-assignment operator has been pushed onto opStack
		    // since the member was parsed (e.g. `ch->fn && (something_else)`),
		    // the `(` belongs to the next sub-expression, not a call through
		    // the fn-ptr. We only count operators with precedence < 14
		    // (anything tighter than `=`); `=` itself is the OUTER context
		    // for declaration init like `int v = (*flfunc)(args)` and must
		    // not block the call.
		    TokenMember *member_call_base = NULL;
		    bool opstack_has_pending_op = false;
		    {
			std::stack<TokenBase *> tmp = opStack;
			while ( !tmp.empty() )
			{
			    TokenBase *t = tmp.top();
			    if ( t->id() == TokenID::tkOpBrk ) { tmp.pop(); continue; }
			    if ( t->is_operator() )
			    {
				int p = ((TokenOperator *)t)->precedence();
				if ( p < 14 )
				{
				    opstack_has_pending_op = true;
				    break;
				}
			    }
			    tmp.pop();
			}
		    }
		    if ( !exStack.empty()
		      && !opstack_has_pending_op
		      && exStack.top()->type() == TokenType::ttMember
		      && (member_call_base = dynamic_cast<TokenMember *>(exStack.top())) != NULL
		      && dynamic_cast<DataDefFPTR *>(member_call_base->var.type) )
		    {
			TokenMember *tmem = member_call_base;
			exStack.pop();
			TokenCallFunc *tc = new TokenCallFunc(tmem->var);
			tc->src_node = tmem;
			tc->file = tb->file;
			tc->line = tb->line;
			tc->column = tb->column;
			tb = parseCallFunc(tc);
			DBG(cout << "member fptr call through " << tmem->var.name << endl);
			opStack.push(tc);
			if ( tb && tb->id() == TokenID::tkSemi )
			    done = true;
			break;
		    }
		    // Direct invocation through a function-pointer variable wrapped
		    // in parens: `(*flfunc)(args)` and `(flfunc)(args)`. After the
		    // inner expression resolved, exStack top is a TokenVar whose
		    // type is DataDefFPTR. The `(` here begins the call args.
		    TokenVar *var_call_base = NULL;
		    if ( !exStack.empty()
		      && !opstack_has_pending_op
		      && exStack.top()->type() == TokenType::ttVariable
		      && (var_call_base = dynamic_cast<TokenVar *>(exStack.top())) != NULL
		      && dynamic_cast<DataDefFPTR *>(var_call_base->var.type) )
		    {
			TokenVar *tv = var_call_base;
			exStack.pop();
			TokenCallFunc *tc = new TokenCallFunc(tv->var);
			tc->file = tb->file;
			tc->line = tb->line;
			tc->column = tb->column;
			tb = parseCallFunc(tc);
			DBG(cout << "var fptr call through " << tv->var.name << endl);
			opStack.push(tc);
			if ( tb && tb->id() == TokenID::tkSemi )
			    done = true;
			break;
		    }
		    ++brackets;
		    DBG(cout << "Got (, pushing onto opStack" << endl);
		    opStack.push(tb); // opStack.push(tb->clone());
		    break;
		}
		// colon stops expression (ternary false branch, case label, range-for)
		if ( tb->id() == TokenID::tkTerC && !brackets )
		{
		    pushToken(tb); // put : back for caller to consume
		    done = true;
		    break;
		}
		if ( tb->id() == TokenID::tkClBrk )
		{
		    if ( !brackets )
		    {
			DBG(cout << "Hit ), no prior brackets, might be end of function?" << endl);
			done = true;
			break;
		    }
		    --brackets;
		    DBG(cout << "Got ), clearing opStack until (" << endl);
		    while ( !opStack.empty() && opStack.top()->get() != '(' )
		    {
			opStack.top()->setFlag(tfBRACKETED);
			popOperator(opStack, exStack);
		    }
		    if ( !opStack.empty() )
			opStack.pop(); // pop off '('
		    if ( conditional && !brackets )
		    {
			TokenBase *next = peekToken();
			// A postfix-chain operator after the close paren means
			// the parenthesized expression is a SUB-expression
			// (e.g. `&((ch)->pcdata->ice_listen)` — the inner
			// `(ch)` closes here but `->pcdata` continues the
			// outer chain). Don't end on stop_on_closing_paren in
			// that case.
			bool postfix_follows = next
			    && (next->id() == TokenID::tkDot
			     || next->id() == TokenID::tkDeRef
			     || next->id() == TokenID::tkOpSqr);
			bool ends_conditional = !postfix_follows && (stop_on_closing_paren || !next
			    || next->id() == TokenID::tkComma
			    || next->id() == TokenID::tkClBrk
			    || next->id() == TokenID::tkClSqr
			    || next->id() == TokenID::tkOpBrc
			    || next->id() == TokenID::tkSemi
			    || next->type() == TokenType::ttKeyword
			    || (ternary_branch && next->id() == TokenID::tkTerC));
			if ( ends_conditional )
			{
			    // Flush any remaining operators before returning so
			    // expressions like `c = -(2)` complete the pending
			    // unary `-` and assignment before the conditional-end
			    // short-circuit. Otherwise exStack may hold only the
			    // inner paren's value, losing the outer operator chain.
			    while ( !opStack.empty() )
				popOperator(opStack, exStack);
			    DBG(std::cout << "Program::parseExpression() conditional end exStack:" << exStack.size() << std::endl);
			    return exStack.empty() ? NULL : exStack.top();
			}
		    }
		    break;
		}
		// ternary operator: condition ? true_expr : false_expr
		if ( tb->id() == TokenID::tkTerQ )
		{
		    DBG(cout << "parseExpression: ternary operator ?" << endl);
		    // pop operators with higher or equal precedence than ? (13)
		    // but NOT assignment (14) or lower precedence operators.
		    // Pending function/method call nodes must also be flushed here so
		    // `func(...) ? a : b` presents the call result as the condition.
		    while ( !opStack.empty() && opStack.top()->get() != '('
			    && ((opStack.top()->type() == TokenType::ttCallFunc
			      || opStack.top()->type() == TokenType::ttCallMethod)
			     || (dynamic_cast<TokenOperator *>(opStack.top())
			      && dynamic_cast<TokenOperator *>(opStack.top())->precedence() <= 13)) )
			popOperator(opStack, exStack);
		    if ( exStack.empty() )
			Throw(tb) << "Missing condition before ?" << flush;
		    TokenTerQ *ternary = (TokenTerQ *)tb;
		    ternary->condition = exStack.top();
		    exStack.pop();
		    // parse true expression — use conditional mode so it stops at : or )
		    // but : is an operator, not ), so we parse then check for :
		    TokenBase *texpr = nextToken();
		    ternary->true_expr = parseExpression(texpr, true, true);
		    // after conditional parseExpression, expect : next
		    TokenBase *colon = nextToken();
		    if ( colon->id() != TokenID::tkTerC )
			Throw(colon) << "Expecting : in ternary expression" << flush;
		    // parse false expression
		    TokenBase *fexpr = nextToken();
		    ternary->false_expr = parseExpression(fexpr, conditional);
		    // Propagate branch datadef up to the ternary so downstream
		    // type-directed paths (e.g. TokenAssign's dtSTRING → char*
		    // coercion) can see a meaningful datadef(). Prefer the true
		    // branch's type; fall back to the false branch if that's
		    // richer (non-NULL / non-int).
		    DataDef *ternary_dd = NULL;
		    if ( ternary->true_expr )
			ternary_dd = ternary->true_expr->datadef();
		    if ( (!ternary_dd || ternary_dd == &ddINT64) && ternary->false_expr )
		    {
			DataDef *fdd = ternary->false_expr->datadef();
			if ( fdd && fdd != &ddINT64 )
			    ternary_dd = fdd;
		    }
		    if ( ternary_dd )
			ternary->setDataType(ternary_dd);
		    // push ternary result onto exStack
		    exStack.push(ternary);
		    // only stop if not inside brackets — inside () we need
		    // to continue to find the closing )
		    if ( brackets == 0 )
			done = true;
		    break;
		}
		// see if we need to convert TokenNeg to TokenSub (binary context)
		if ( tb->id() == TokenID::tkNeg && isPostfixPosition() )
		{
		    DBG(std::cout << "parseExpression() converting TokenNeg to TokenSub, prevToken id: " << (int)prevToken()->id() << " prevToken->is_operator: " << (prevToken()->is_operator() ? "true" : "false") << std::endl);
		    TokenSub *ts = new TokenSub();

		    ts->file = tb->file;
		    ts->line = tb->line;
		    ts->column = tb->column;
		    // should we delete tb ?
		    tb = ts;
		}
		// & address-of in unary position
		if ( tb->id() == TokenID::tkBand && isUnaryPosition() )
		{
		    // unary & — address-of operator. Accept `&name` or `&(name)`;
		    // the parenthesized form shows up after macro expansion
		    // (e.g. `FD_SET(fd, set)` → `__madc_fd_set(fd, &(set))`).
		    bool paren = false;
		    if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
		    {
			nextToken(); // consume '('
			paren = true;
		    }
		    TokenBase *addr_tb = nextToken();
		    TokenBase *addr_expr = NULL;
		    if ( paren )
		    {
			addr_expr = parseExpression(addr_tb, true, false, true);
			TokenBase *close = nextToken();
			if ( !close || close->id() != TokenID::tkClBrk )
			    Throw(close ? close : addr_tb) << "expected ')' after &(name)" << flush;
			if ( dynamic_cast<TokenMember *>(addr_expr)
			       || dynamic_cast<TokenDeref *>(addr_expr)
			       || dynamic_cast<TokenSubscript *>(addr_expr)
			       || dynamic_cast<TokenSubscriptExpr *>(addr_expr)
			       || dynamic_cast<TokenDerefExpr *>(addr_expr) )
			{
			    DataDefPTR *aptr = getPointerType(addr_expr->datadef());
			    exStack.push(new TokenAddrExpr(addr_expr, aptr));
			}
			else if ( TokenVar *tv = dynamic_cast<TokenVar *>(addr_expr) )
			{
			    tv->var.flags |= vfADDRTAKEN;
			    DataDefPTR *aptr = getPointerType(tv->var.type);
			    exStack.push(new TokenAddrOf(tv->var, aptr));
			}
			else
			    Throw(addr_tb) << "expecting addressable expression after '&('" << flush;
		    }
		    else
		    {
			bool postfix_chain = is_contextual_identifier_token(addr_tb)
			    && peekToken()
			    && (peekToken()->id() == TokenID::tkDot
			     || peekToken()->id() == TokenID::tkDeRef
			     || peekToken()->id() == TokenID::tkOpSqr);
			if ( postfix_chain )
			{
			    addr_expr = parseExpression(addr_tb, true, false, false, 0);
			    if ( dynamic_cast<TokenMember *>(addr_expr)
			       || dynamic_cast<TokenDeref *>(addr_expr)
			       || dynamic_cast<TokenSubscript *>(addr_expr)
			       || dynamic_cast<TokenSubscriptExpr *>(addr_expr)
			       || dynamic_cast<TokenDerefExpr *>(addr_expr) )
			    {
				DataDefPTR *aptr = getPointerType(addr_expr->datadef());
				exStack.push(new TokenAddrExpr(addr_expr, aptr));
			    }
			    else if ( TokenVar *tv = dynamic_cast<TokenVar *>(addr_expr) )
			    {
				tv->var.flags |= vfADDRTAKEN;
				DataDefPTR *aptr = getPointerType(tv->var.type);
				exStack.push(new TokenAddrOf(tv->var, aptr));
			    }
			    else
				Throw(addr_tb) << "expecting addressable expression after '&'" << flush;
			}
			else
			{
			    if ( !is_contextual_identifier_token(addr_tb) )
				Throw(addr_tb) << "expecting variable name after '&'" << flush;
			    std::string aname = contextual_identifier_name(addr_tb);
			    Variable *avar = findVariable(aname);
			    if ( !avar )
				Throw(addr_tb) << "undeclared identifier '" << aname << "'" << flush;
			    avar->flags |= vfADDRTAKEN;
			    DataDefPTR *aptr = getPointerType(avar->type);
			    exStack.push(new TokenAddrOf(*avar, aptr));
			}
		    }
		    break;
		}
			// * dereference in unary position
			if ( tb->id() == TokenID::tkMul && isUnaryPosition() )
			{
			    TokenBase *deref_tb = nextToken();
			    if ( deref_tb->id() == TokenID::tkOpBrk )
		    {
			// Check whether the inner expression is a cast
			// signature (`(TYPE*) expr`). If so, delegate the
			// whole `(...)` to parseExpression so its cast
			// detection fires and `TYPE` gets resolved against
			// `datatype_map` instead of being sent through the
			// identifier/variable lookup path — which fails for
			// typedef'd struct names like `EXT_BV` in
			// `*(EXT_BV*)vd.data`. Delegation consumes the `)`
			// itself, so the subsequent nextToken() is skipped
			// on the cast path.
			TokenBase *peek_inner = peekToken();
			bool inner_is_cast_head =
			    peek_inner
			    && ( peek_inner->type() == TokenType::ttDataType
			      || peek_inner->id() == TokenID::tkSTRUCT
			      || peek_inner->id() == TokenID::tkCLASS
			      || ( peek_inner->type() == TokenType::ttIdentifier
				&& datatype_map.count(((TokenIdent *)peek_inner)->str) ) );
			TokenBase *deref_expr;
			if ( inner_is_cast_head )
			{
			    deref_expr = parseExpression(deref_tb, true);
			}
			else
			{
			    TokenBase *inner_tb = nextToken();
			    deref_expr = parseExpression(inner_tb, true);
			    TokenBase *close = nextToken();
			    if ( !close || close->id() != TokenID::tkClBrk )
				Throw(close ? close : deref_tb) << "expected ')' after *(expr)" << flush;
			}
			DataDef *dtype = deref_expr->datadef();
			if ( !dtype )
			    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
			if ( dtype->is_function() && dtype->is_numeric() )
			{
			    exStack.push(deref_expr);
			    break;
			}
			if ( !dtype->is_pointer() )
			    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
			DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dtype);
			DataDef *base = dptr ? dptr->base_type : &ddINT64;
			exStack.push(new TokenDerefExpr(deref_expr, base));
			    }
			    else
			    {
				if ( deref_tb->type() == TokenType::ttIdentifier
				  && !(peekToken()
				    && (peekToken()->id() == TokenID::tkOpBrk
				     || peekToken()->id() == TokenID::tkDeRef
				     || peekToken()->id() == TokenID::tkDot
				     || peekToken()->id() == TokenID::tkOpSqr)) )
				{
				    std::string dname = ((TokenIdent *)deref_tb)->str;
				    Variable *dvar = findVariable(dname);
				    if ( !dvar )
					Throw(deref_tb) << "undeclared identifier '" << dname << "'" << flush;
				    // C function-to-pointer decay reverses through `*`:
				    // `*fp` (where fp is a function pointer) IS the
				    // function — still callable as `(*fp)(args)`. Push
				    // the variable as a value and let the call-site
				    // logic dispatch normally.
				    if ( dvar->type->is_function() && dvar->type->is_numeric() )
				    {
					exStack.push(new TokenVar(*dvar));
					break;
				    }
				    if ( !dvar->type->is_pointer() && !dvar->is_fixed_array() )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    DataDef *base = dvar->type;
				    if ( dvar->type->is_pointer() )
				    {
					DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dvar->type);
					base = dptr ? dptr->base_type : &ddINT64;
				    }
				    if ( peekToken() && (peekToken()->id() == TokenID::tkInc || peekToken()->id() == TokenID::tkDec) )
				    {
					TokenBase *step_tb = nextToken();
					TokenBase *step_expr = new TokenDerefStep(*dvar, base, step_tb->id() == TokenID::tkInc);
					exStack.push(step_expr);
					_cur_token = step_expr;
				    }
				    else
					exStack.push(new TokenDeref(*dvar, base));
				}
			    else
			    {
				TokenBase *deref_expr = NULL;
				if ( deref_tb->id() == TokenID::tkMul )
				{
				    TokenBase *inner_tb = nextToken();
				    if ( !inner_tb )
					Throw(deref_tb) << "expecting pointer expression after '*'" << flush;
				    if ( inner_tb->id() == TokenID::tkOpBrk )
				    {
					TokenBase *inner_expr_tb = nextToken();
					TokenBase *inner_expr = parseExpression(inner_expr_tb, true);
					TokenBase *close = nextToken();
					if ( !close || close->id() != TokenID::tkClBrk )
					    Throw(close ? close : inner_tb) << "expected ')' after *(expr)" << flush;
					DataDef *inner_dtype = inner_expr ? inner_expr->datadef() : NULL;
					if ( !inner_dtype || !inner_dtype->is_pointer() )
					    Throw(inner_tb) << "cannot dereference non-pointer type" << flush;
					DataDefPTR *inner_dptr = dynamic_cast<DataDefPTR *>(inner_dtype);
					DataDef *inner_base = inner_dptr ? inner_dptr->base_type : &ddINT64;
					deref_expr = new TokenDerefExpr(inner_expr, inner_base);
				    }
				    else if ( inner_tb->type() == TokenType::ttIdentifier
					  && !(peekToken()
					    && (peekToken()->id() == TokenID::tkOpBrk
					     || peekToken()->id() == TokenID::tkDeRef
					     || peekToken()->id() == TokenID::tkDot
					     || peekToken()->id() == TokenID::tkOpSqr)) )
				    {
					std::string inner_name = ((TokenIdent *)inner_tb)->str;
					Variable *inner_var = findVariable(inner_name);
					if ( !inner_var )
					    Throw(inner_tb) << "undeclared identifier '" << inner_name << "'" << flush;
					if ( !inner_var->type->is_pointer() && !inner_var->is_fixed_array() )
					    Throw(inner_tb) << "cannot dereference non-pointer type" << flush;
					DataDef *inner_base = inner_var->type;
					if ( inner_var->type->is_pointer() )
					{
					    DataDefPTR *inner_dptr = dynamic_cast<DataDefPTR *>(inner_var->type);
					    inner_base = inner_dptr ? inner_dptr->base_type : &ddINT64;
					}
					deref_expr = new TokenDeref(*inner_var, inner_base);
				    }
				    else
					Throw(inner_tb) << "expecting pointer expression after '*'" << flush;
				}
				else if ( deref_tb->type() == TokenType::ttIdentifier
				   && peekToken()
				   && (peekToken()->id() == TokenID::tkDeRef
				    || peekToken()->id() == TokenID::tkDot
				    || peekToken()->id() == TokenID::tkOpSqr) )
				{
				    // Postfix chain (e.g. `res->name`, `p.x`, `tab[i]`)
				    // — parse only the chain so trailing binary operators
				    // like `*p->name == '$'` don't get swallowed.
				    deref_expr = parsePostfixChain(deref_tb);
				}
				else if ( (deref_tb->id() == TokenID::tkInc
				        || deref_tb->id() == TokenID::tkDec)
				    && peekToken()
				    && peekToken()->type() == TokenType::ttIdentifier )
				{
				    // Pre-increment / pre-decrement of a pointer:
				    // `*++p`, `*--p`. The recursive parseExpression
				    // path would happily consume any trailing binary
				    // operator (`*++p == 'e'` would parse as
				    // `*(++p == 'e')`), so handle the unary step
				    // explicitly: build a `TokenInc(right=p)` (pre-
				    // increment, which mutates p and yields its new
				    // value), then wrap it in a `TokenDerefExpr` so
				    // the deref reads through the post-step pointer.
				    TokenBase *id_tb = nextToken();
				    std::string id_name = ((TokenIdent *)id_tb)->str;
				    Variable *id_var = findVariable(id_name);
				    if ( !id_var )
					Throw(id_tb) << "undeclared identifier '" << id_name << "'" << flush;
				    if ( !id_var->type->is_pointer() )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    DataDefPTR *idptr = dynamic_cast<DataDefPTR *>(id_var->type);
				    DataDef *base = (idptr && idptr->base_type) ? idptr->base_type : &ddINT64;
				    TokenOperator *step;
				    if ( deref_tb->id() == TokenID::tkInc )
					step = new TokenInc();
				    else
					step = new TokenDec();
				    step->left = NULL;
				    step->right = new TokenVar(*id_var);
				    deref_expr = new TokenDerefExpr(step, base);
				    DataDef *dtype = id_var->type;
				    if ( !dtype || !dtype->is_pointer() )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    exStack.push(deref_expr);
				    break;
				}
				else
				    deref_expr = parseExpression(deref_tb, true);
				if ( !deref_expr )
				    Throw(deref_tb) << "expecting pointer expression after '*'" << flush;
				DataDef *dtype = deref_expr->datadef();
				if ( !dtype )
				    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				if ( dtype->is_function() && dtype->is_numeric() )
				{
				    exStack.push(deref_expr);
				    break;
				}
				if ( !dtype->is_pointer() )
				    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dtype);
				DataDef *base = dptr ? dptr->base_type : &ddINT64;
				exStack.push(new TokenDerefExpr(deref_expr, base));
				}
			    }
			    break;
			}
		if ( tb->id() == TokenID::tkDec || tb->id() == TokenID::tkInc )
		{
		    DBG(cout << "parseExpression: Got operator: " << (char)tb->get() << (char)tb->get() << endl);
		    to = (TokenOperator *)tb;
		    to->left = NULL;
		    to->right = NULL;
		    if ( isPostfixPosition() && !exStack.empty() )
		    {
			to->left = exStack.top(); exStack.pop(); DBG(cout << "popped " << to->left->ival() << endl);
			exStack.push(to);
		    }
		    else
			opStack.push(to);
		    break;
		}
		DBG(cout << "parseExpression: Got operator: " << (char)tb->get() << " id() " << (int)tb->id() << endl);
		to = (TokenOperator *)tb; // ->clone();
		to->left = NULL;
		to->right = NULL;
		// whiile: there is a function at the top of the operator stack)
		// or (there is an operator at the top of the operator stack with greater precedence)
		// or (the operator at the top of the operator stack has equal precedence and is left associative))
		// and (the operator at the top of the operator stack is not a left parenthesis):
		// (Note: we don't put functions in the stack right now)
		while ( !opStack.empty() && opStack.top()->id() != TokenID::tkOpBrk
		&&      (opStack.top()->type() == TokenType::ttCallFunc || opStack.top()->type() == TokenType::ttCallMethod
		||      (opStack.top()->is_operator() && (*((TokenOperator *)opStack.top()) > *to))) )
		{
		    DBG(cout << "Operator(" << (char)opStack.top()->get() << ") has precedence over operator(" << (char)to->get() << ')' << endl);
		    popOperator(opStack, exStack);
		}
		DBG(cout << "Pushing " << (char)tb->get() << " onto opStack" << endl);
		opStack.push(to);
		break;
            case TokenType::ttDataType:
		bt = (TokenDataType *)tb;
		// If the next token isn't an identifier, the user probably
		// meant the data type *name* as a contextual identifier — a
		// parameter or local variable named e.g. `string`. Look it up
		// as a variable first; only treat as inline declaration if we
		// don't find one and the next token is an identifier.
		if ( is_contextual_identifier_token(bt) )
		{
		    std::string ctx_name = contextual_identifier_name(bt);
		    Variable *ctx_var = findVariable(ctx_name);
		    if ( ctx_var
			 && (!peekToken() || peekToken()->type() != TokenType::ttIdentifier) )
		    {
			DBG(cout << "ttDataType " << ctx_name << " resolves to variable" << endl);
			exStack.push(new TokenVar(*ctx_var));
			break;
		    }
		}
		tb = nextToken();
		if ( tb->type() != TokenType::ttIdentifier ) { Throw(tb) << "Expecting identifier" << flush; }
		var = addVariable(code, bt->definition, ((TokenIdent *)tb)->str);
		DBG(cout << "Pushing newly declared variable: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttString:
		var = addLiteral(((TokenIdent *)tb)->str);
		DBG(cout << "Pushing new variable of literal: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttKeyword:
		if ( !is_contextual_identifier_token(tb) )
		    Throw(tb) << "Unexpected keyword in expression" << flush;
	    case TokenType::ttIdentifier:
	    {
		std::string contextual_name = contextual_identifier_name(tb);
		TokenIdent contextual_ident(contextual_name);
		TokenIdent *ident_tb = tb->type() == TokenType::ttIdentifier
		    ? (TokenIdent *)tb
		    : &contextual_ident;
		// sizeof(type) — resolve to integer constant at parse time
		if ( ident_tb->str == "sizeof" )
		{
		    // C also accepts `sizeof expr` (no parens) for unary
		    // expressions: `sizeof ok_otype`, `sizeof *a`, `sizeof r`.
		    // When the follower isn't `(`, parse a postfix chain (or
		    // a `*` deref of one) and use the result's datadef size.
		    if ( peekToken() && peekToken()->id() != TokenID::tkOpBrk )
		    {
			TokenBase *probe = peekToken();
			bool deref = (probe->id() == TokenID::tkMul);
			if ( deref )
			{
			    nextToken(); // consume `*`
			    probe = peekToken();
			}
			if ( !probe || !is_contextual_identifier_token(probe) )
			    Throw(tb) << "Expecting '(' or identifier after sizeof" << flush;
			TokenBase *id_tb = nextToken();
			TokenBase *chain = parsePostfixChain(id_tb);
			DataDef *cdd = chain ? chain->datadef() : NULL;
			if ( !cdd )
			    Throw(id_tb) << "sizeof: cannot determine type of expression" << flush;
			size_t sz = cdd->size;
			if ( deref && cdd->is_pointer() )
			{
			    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(cdd);
			    if ( pdd && pdd->base_type )
				sz = pdd->base_type->size;
			}
			else if ( !deref )
			{
			    if ( TokenVar *tv = dynamic_cast<TokenVar *>(chain) )
			    {
				if ( tv->var.is_fixed_array() )
				    sz = tv->var.type->size * tv->var.total_elements();
			    }
			}
			TokenInt *ti = new TokenInt((int64_t)sz);
			ti->file = tb->file; ti->line = tb->line; ti->column = tb->column;
			exStack.push(ti);
			break;
		    }
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after sizeof" << flush;
		    nextToken(); // consume (
		    TokenBase *type_tb = nextToken(); // consume type
		    DataDef *dd = NULL;
		    Variable *var = NULL;
		    size_t sizeof_value = 0;
		    if ( type_tb->type() == TokenType::ttDataType )
			dd = &((TokenDataType *)type_tb)->definition;
		    else if ( type_tb->type() == TokenType::ttIdentifier )
		    {
			std::string tname = ((TokenIdent *)type_tb)->str;
			var = findVariable(tname);
			// sizeof(expr) — postfix chain (`buf[0]`, `obj.field`,
			// `ptr->field`). Take the chain's result datadef's size.
			if ( var && peekToken()
			  && (peekToken()->id() == TokenID::tkOpSqr
			   || peekToken()->id() == TokenID::tkDot
			   || peekToken()->id() == TokenID::tkDeRef) )
			{
			    TokenBase *chain = parsePostfixChain(type_tb);
			    DataDef *cdd = chain ? chain->datadef() : NULL;
			    if ( !cdd )
				Throw(type_tb) << "sizeof: cannot determine type of expression" << flush;
			    sizeof_value = cdd->size;
			    var = NULL;
			}
			else if ( var )
			{
			    sizeof_value = var->type->size;
			    if ( var->is_fixed_array() )
				sizeof_value *= var->total_elements();
			}
			// check struct_map
			if ( !var && !sizeof_value )
			{
			    datadef_map_iter dmi = struct_map.find(tname);
			    if ( dmi != struct_map.end() )
				dd = dmi->second;
			    // check datatype_map
			    if ( !dd )
			    {
				datatype_map_iter bmi = datatype_map.find(tname);
				if ( bmi != datatype_map.end() )
				    dd = &bmi->second->definition;
			    }
			    // check lazy types
			    if ( !dd )
				dd = lazy_resolve_type(tname);
			}
		    }
		    else if ( type_tb->type() == TokenType::ttKeyword && type_tb->id() == TokenID::tkSTRUCT )
		    {
			// sizeof(struct tag)
			TokenBase *tag_tb = nextToken();
			if ( tag_tb->type() == TokenType::ttIdentifier )
			{
			    std::string tname = ((TokenIdent *)tag_tb)->str;
			    datadef_map_iter dmi = struct_map.find(tname);
			    if ( dmi != struct_map.end() )
				dd = dmi->second;
			}
			if ( !dd )
			    Throw(tag_tb) << "Unknown struct type in sizeof" << flush;
		    }
		    else if ( type_tb->id() == TokenID::tkMul )
		    {
			// sizeof(*identifier) and sizeof(*expr->member) — element
			// size of a pointer or fixed array. Standard C idioms:
			//   sizeof(arr) / sizeof(*arr)        — count elements
			//   imc_malloc(sizeof(*c->local))     — allocate one
			TokenBase *deref_tb = nextToken();
			if ( !deref_tb || !is_contextual_identifier_token(deref_tb) )
			    Throw(type_tb) << "Expecting identifier after '*' in sizeof" << flush;
			DataDef *deref_base = NULL;
			// `*ident.member` / `*ident->member` / `*ident[i]` — postfix
			// chain. parsePostfixChain hands back a fully resolved node
			// whose datadef() is the chain's value type. For
			// sizeof(*chain) we want the dereffed type's size — if the
			// chain is a pointer, sizeof(*chain) = sizeof(pointed-to);
			// if it's a fixed array, sizeof(*chain) = sizeof(element).
			if ( peekToken()
			  && (peekToken()->id() == TokenID::tkDot
			   || peekToken()->id() == TokenID::tkDeRef
			   || peekToken()->id() == TokenID::tkOpSqr) )
			{
			    TokenBase *chain = parsePostfixChain(deref_tb);
			    DataDef *cdd = chain ? chain->datadef() : NULL;
			    if ( !cdd )
				Throw(deref_tb) << "sizeof(*expr): cannot determine type" << flush;
			    if ( cdd->is_pointer() )
			    {
				DataDefPTR *cdp = dynamic_cast<DataDefPTR *>(cdd);
				deref_base = (cdp && cdp->base_type) ? cdp->base_type : &ddINT64;
			    }
			    else
				deref_base = cdd;
			    sizeof_value = deref_base ? deref_base->size : 8;
			}
			else
			{
			    std::string dname = contextual_identifier_name(deref_tb);
			    Variable *dvar = findVariable(dname);
			    if ( !dvar )
				Throw(deref_tb) << "undeclared identifier '" << dname << "' in sizeof(*...)" << flush;
			    if ( dvar->is_fixed_array() )
			    {
				// *arr where arr is a fixed array: element type size.
				sizeof_value = dvar->type->size;
			    }
			    else if ( dvar->type->is_pointer() )
			    {
				DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dvar->type);
				DataDef *base = (dptr && dptr->base_type) ? dptr->base_type : &ddINT64;
				sizeof_value = base->size;
			    }
			    else
				Throw(deref_tb) << "sizeof(*" << dname << "): not a pointer or array" << flush;
			}
		    }
		    if ( !var && !dd && !sizeof_value )
			Throw(type_tb) << "Unknown type in sizeof" << flush;
		    // handle pointer: sizeof(type *)
		    while ( !var && peekToken() && peekToken()->id() == TokenID::tkMul )
		    {
			nextToken(); // consume '*'
			dd = getPointerType(dd);
		    }
		    // consume closing )
		    if ( !peekToken() || peekToken()->id() != TokenID::tkClBrk )
			Throw(type_tb) << "Expecting ')' after sizeof type" << flush;
		    nextToken(); // consume )
		    if ( !var && !sizeof_value && dd )
			sizeof_value = dd->size;
		    exStack.push(new TokenInt((int)sizeof_value));
		    break;
		}
		// va_arg(ap, type) — compiler intrinsic for reading variadic args
		if ( ident_tb->str == "va_arg" )
		{
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after va_arg" << flush;
		    nextToken(); // consume (
		    // first arg: the va_list variable name
		    TokenBase *ap_tb = nextToken();
		    if ( ap_tb->type() != TokenType::ttIdentifier )
			Throw(ap_tb) << "Expecting va_list variable name in va_arg" << flush;
		    std::string ap_name = ((TokenIdent *)ap_tb)->str;
		    TokenCpnd *scope = compounds.empty() ? NULL : compounds.top();
		    Variable *ap_var = scope ? scope->findVariable(ap_name) : NULL;
		    if ( !ap_var )
			ap_var = findVariable(ap_name);
		    if ( !ap_var )
			Throw(ap_tb) << "Unknown variable '" << ap_name << "' in va_arg" << flush;
		    // consume comma
		    TokenBase *comma_tb = nextToken();
		    if ( comma_tb->id() != TokenID::tkComma )
			Throw(comma_tb) << "Expecting ',' after va_list variable in va_arg" << flush;
		    // second arg: type name
		    TokenBase *type_tb = nextToken();
		    DataDef *target_dd = NULL;
		    if ( type_tb->type() == TokenType::ttDataType )
			target_dd = &((TokenDataType *)type_tb)->definition;
		    else if ( type_tb->type() == TokenType::ttIdentifier )
		    {
			std::string tname = ((TokenIdent *)type_tb)->str;
			datatype_map_iter tdmi = datatype_map.find(tname);
			if ( tdmi != datatype_map.end() )
			    target_dd = &tdmi->second->definition;
			if ( !target_dd )
			{
			    datadef_map_iter sdmi = struct_map.find(tname);
			    if ( sdmi != struct_map.end() )
				target_dd = sdmi->second;
			}
		    }
		    if ( !target_dd )
			Throw(type_tb) << "Unknown type in va_arg" << flush;
		    // handle pointer: va_arg(ap, char *)
		    while ( peekToken() && peekToken()->id() == TokenID::tkMul )
		    {
			nextToken(); // consume '*'
			target_dd = getPointerType(target_dd);
		    }
		    // consume closing )
		    if ( !peekToken() || peekToken()->id() != TokenID::tkClBrk )
			Throw(type_tb) << "Expecting ')' after va_arg type" << flush;
		    nextToken(); // consume )
		    exStack.push(new TokenVaArg(ap_var, target_dd));
		    break;
		}
	    	if ( prevToken() && prevToken()->id() == TokenID::tkDot )
		{
#if 0
		    DBG(cout << "parseExpression() prevToken is tkDot, pushing TokenIdent " << ((TokenIdent *)tb)->str << endl);
		    exStack.push(tb);
#else
		    if ( exStack.empty() )
			Throw(tb) << "expected expression" << flush;
		    // Accept TokenVar, TokenMember, or TokenSubscript as LHS for dot access.
		    // Subscript case: tab[i].member for an array of structs.
		    TokenBase *lhs_dot = exStack.top();
		    if ( lhs_dot->type() != TokenType::ttVariable
		      && lhs_dot->type() != TokenType::ttMember
		      && lhs_dot->type() != TokenType::ttSubscript )
			Throw(tb) << "member reference is not a structure or union" << flush;
		    Variable *tv_var;
		    DataDef  *struct_type;
		    if ( lhs_dot->type() == TokenType::ttVariable )
		    {
			TokenVar *tv = dynamic_cast<TokenVar *>(lhs_dot);
			tv_var      = &tv->var;
			struct_type =  tv->var.type;
		    }
		    else if ( lhs_dot->type() == TokenType::ttMember )
		    {
			TokenMember *tm = dynamic_cast<TokenMember *>(lhs_dot);
			if ( tm )
			{
			    tv_var      = &tm->var;
			    struct_type =  tm->var.type;
			}
			else if ( TokenDeref *tdl = dynamic_cast<TokenDeref *>(lhs_dot) )
			{
			    // (*p).member — logically equivalent to p->member.
			    // Route the dot through the pointer variable so the
			    // normal TokenMember pointer-in-Gp path compiles it
			    // as [p + offset] at codegen time.
			    tv_var      = &tdl->var;
			    struct_type =  tdl->deref_type;
			}
			else if ( TokenDerefExpr *tdxl = dynamic_cast<TokenDerefExpr *>(lhs_dot) )
			{
			    // (*expr).member — expr yields a pointer whose target
			    // is a struct. Resolve member lookup against the
			    // dereferenced struct type; codegen uses the expr's
			    // pointer value as base via TokenMember's parent_expr
			    // path.
			    tv_var      = new Variable("__deref_expr", *tdxl->deref_type, 1, NULL, false);
			    struct_type =  tdxl->deref_type;
			}
			else
			    Throw(tb) << "member reference '.' on unsupported deref expression" << flush;
		    }
		    else
		    {
			TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(lhs_dot);
			if ( tsub )
			{
			    tv_var      = &tsub->object;
			    struct_type =  tsub->datadef(); // element type
			}
			else if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(lhs_dot) )
			{
			    // expr[i].member — subscript LHS is an arbitrary
			    // pointer/array-producing expression (e.g.
			    // `ch->pcdata->killed[x].vnum`). Synthesize a
			    // struct-typed object variable and route codegen
			    // through the TokenSubscriptExpr as parent_expr;
			    // TokenMember::operand's dot-chain path handles
			    // [base + idx*shift + offset].
			    DataDef *elem_type = tse->datadef();
			    if ( !elem_type )
				Throw(tb) << "subscript expression has no element type" << flush;
			    tv_var      = new Variable("__sub_expr", *elem_type, 1, NULL, false);
			    struct_type =  elem_type;
			}
			else
			    Throw(tb) << "member reference '.' on unsupported subscript form" << flush;
		    }
		    if ( !struct_type->is_struct() && !struct_type->is_object() )
			Throw(tb) << "member reference is not a structure or union" << flush;
		    var = NULL;
		    string id = ident_tb->str;
		    if ( struct_type->is_object() && (var=((DataDefCLASS *)struct_type)->findMethod(id)) )
		    {
			if ( lhs_dot->type() != TokenType::ttVariable )
			    Throw(tb) << "chained method call not yet supported" << flush;
			TokenVar *tv = dynamic_cast<TokenVar *>(lhs_dot);
			// cout << "Found " << tv->var.name << "::" << var->name << endl;
			TokenCallMethod *tc = new TokenCallMethod(tv->var, *var);
			tb = nextToken();
			tc->line = tb->line;
			tc->column = tb->column;
			// if bracket, parse params
			if ( tb->id() == TokenID::tkOpBrk )
			{
			    // delete tb?
			    tb = parseCallMethod(tc);
			    DBG(cout << "parseCallMethod returned with token " << (char)tb->get() << endl);
			}
			// remove object TokenVar from exStack
			exStack.pop();
			// remove TokenDot from opStack
			if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDot )
			{
			    DBG(cout << "parseCallMethod, removing tkDot from opStack" << endl);
			    opStack.pop();
			}
			DBG(cout << "Pushing found method call: " << var->name << "() onto opStack" << endl);
			opStack.push(tc);
			// I'm not sure why I need to do this TODO: figure this out
			if ( tb->id() == TokenID::tkSemi )
			    done = true;
			break;
		    }
		    // get offset
		    ssize_t ofs = ((DataDefSTRUCT *)struct_type)->m_offset(id);
		    if ( ofs == -1 )
			Throw(tb) << "Unidentified member" << flush;
		    DataDef *mtype = ((DataDefSTRUCT *)struct_type)->m_type(id);
		    // create new variable
		    var = new Variable(id, *mtype, 1, NULL, false);
		    var->flags = tv_var->flags;
		    if ( tv_var->data )
			var->data = (void *)((char *)tv_var->data + ofs);
		    // remove LHS from exStack
		    exStack.pop();
		    // When LHS carries its own base-pointer (TokenMember for dot/arrow
		    // chains, or TokenSubscript for array-of-structs), pass it as
		    // parent_expr so TokenMember::operand() can resolve the base at
		    // codegen time.
		    //
		    // TokenDeref reports ttMember for LHS-compat reasons but is
		    // not a real TokenMember — for `(*p).x` we already routed
		    // tv_var to the pointer `p`, so the normal no-parent_expr path
		    // compiles it as `p->x` via voperand's pointer-in-Gp branch.
		    //
		    // TokenDerefExpr is the opposite: `(*expr).x` where expr is a
		    // pointer-producing expression. Pass the TokenDerefExpr as
		    // parent_expr so TokenMember::operand calls expr->compile to
		    // materialize the pointer value at codegen, then accesses
		    // [ptr + offset] via the struct-value ("dot chain") branch.
		    bool is_deref_lhs = (dynamic_cast<TokenDeref *>(lhs_dot) != NULL);
		    bool is_derefexpr_lhs = (dynamic_cast<TokenDerefExpr *>(lhs_dot) != NULL);
		    if ( is_derefexpr_lhs )
			exStack.push(new TokenMember(*tv_var, *var, ofs, lhs_dot));
		    else if ( !is_deref_lhs
		      && (lhs_dot->type() == TokenType::ttMember
		       || lhs_dot->type() == TokenType::ttSubscript) )
			exStack.push(new TokenMember(*tv_var, *var, ofs, lhs_dot));
		    else
			exStack.push(new TokenMember(*tv_var, *var, ofs));
		    // remove TokenDot from opStack
		    if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDot )
			opStack.pop();
#endif
		    break;
		}
		// -> pointer member access: ptr->member
		if ( prevToken() && prevToken()->id() == TokenID::tkDeRef )
		{
		    if ( exStack.empty() )
			Throw(tb) << "expected expression before '->'" << flush;

		    // get the pointer-valued LHS — from TokenVar/TokenMember or a
		    // pointer-returning subscript expression such as tab[i]->field
		    TokenBase *lhs = exStack.top();
		    Variable *obj_var = NULL;
		    DataDef *obj_type = NULL;
		    bool expr_backed_lhs = false;
		    if ( lhs->type() == TokenType::ttVariable )
		    {
			obj_var = &dynamic_cast<TokenVar *>(lhs)->var;
			obj_type = obj_var->type;
		    }
		    else if ( lhs->type() == TokenType::ttMember )
		    {
			// TokenDeref and TokenDerefExpr also report ttMember (reuse
			// member type for assignment compat) but are not TokenMember
			// instances. When the cast fails, fall through to the
			// expression-backed path using the node's reported datadef
			// instead of throwing.
			TokenMember *tm = dynamic_cast<TokenMember *>(lhs);
			if ( tm )
			{
			    obj_var = &tm->var;
			    obj_type = tm->var.type;
			}
			else if ( lhs->datadef() && lhs->datadef()->is_pointer() )
			{
			    obj_type = lhs->datadef();
			    obj_var = new Variable("__arrow_expr", *obj_type, 1, NULL, false);
			    expr_backed_lhs = true;
			}
			else
			    Throw(tb) << "expression before '->' must be a pointer to struct" << flush;
		    }
		    else if ( lhs->datadef() && lhs->datadef()->is_pointer() )
		    {
			obj_type = lhs->datadef();
			obj_var = new Variable("__arrow_expr", *obj_type, 1, NULL, false);
			expr_backed_lhs = true;
		    }
		    else
			Throw(tb) << "expression before '->' must be a pointer" << flush;

		    if ( !obj_type->is_pointer() )
			Throw(tb) << "expression before '->' must be a pointer" << flush;

		    // get the pointed-to type
		    DataDefPTR *ptr_type = dynamic_cast<DataDefPTR *>(obj_type);
		    if ( !ptr_type || !ptr_type->base_type )
			Throw(tb) << "expression before '->' is not a typed pointer" << flush;
		    DataDef *base = ptr_type->base_type;
		    if ( !base->is_struct() && !base->is_object() )
			Throw(tb) << "member reference type is not a structure or union" << flush;

		    string id = ident_tb->str;

		    // get member offset and type
		    ssize_t ofs = ((DataDefSTRUCT *)base)->m_offset(id);
		    if ( ofs == -1 )
			Throw(tb) << "no member named '" << id << "'" << flush;
		    DataDef *mtype = ((DataDefSTRUCT *)base)->m_type(id);

		    // create variable for the member
		    var = new Variable(id, *mtype, 1, NULL, false);
		    var->flags = obj_var->flags;

		    // remove LHS from exStack
		    exStack.pop();
		    // for chained -> (lhs was a TokenMember), pass it as parent_expr so
		    // operand() can compile the intermediate pointer at codegen time
		    if ( lhs->type() == TokenType::ttMember || expr_backed_lhs )
			exStack.push(new TokenMember(*obj_var, *var, ofs, lhs));
		    else
			exStack.push(new TokenMember(*obj_var, *var, ofs));
		    // remove TokenDeRef from opStack
		    if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDeRef )
			opStack.pop();
		    break;
		}
		// namespace resolution: identifier :: member
		if ( peekToken() && peekToken()->id() == TokenID::tkNS )
		{
		    std::string ns_name = ident_tb->str;
		    namespace_map_t::iterator nsi = namespace_map.find(ns_name);
		    if ( nsi == namespace_map.end() )
			Throw(tb) << "Unknown namespace '" << ns_name << "'" << flush;
		    nextToken(); // consume '::'
		    TokenBase *member_tb = nextToken(); // consume member identifier
		    if ( !member_tb || member_tb->type() != TokenType::ttIdentifier )
			Throw(tb) << "Expecting identifier after '" << ns_name << "::'" << flush;
		    std::string member_name = ((TokenIdent *)member_tb)->str;
		    variable_map_iter vmi = nsi->second.find(member_name);
		    if ( vmi == nsi->second.end() )
		    {
			// try dlsym fallback if this namespace was loaded via #load
			std::map<std::string, void *>::iterator dli = dlopen_map.find(ns_name);
			if ( dli == dlopen_map.end() )
			    Throw(member_tb) << "'" << member_name << "' is not a member of namespace '" << ns_name << "'" << flush;
			void *sym = dlsym(dli->second, member_name.c_str());
			if ( !sym )
			    Throw(member_tb) << "dlsym failed for '" << member_name << "' in '" << ns_name << "': " << dlerror() << flush;
			// create function with int64 return, no declared params (variadic-like)
			// actual args are passed through at compile time
			std::string func_id = "__dl_" + ns_name + "_" + member_name;
			var = addFunction(func_id,
			    datatype_vec_t{DataType::dtINT64},
			    (fVOIDFUNC)sym);
			if ( !var )
			    Throw(member_tb) << "Failed to register dlsym function '" << member_name << "'" << flush;
			nsi->second[member_name] = var; // cache for next call
			DBG(cout << "parseExpression() dlsym resolved " << ns_name << "::" << member_name << " at " << (uint64_t)sym << endl);
		    }
		    else
			var = vmi->second;
		    DBG(cout << "parseExpression() resolved " << ns_name << "::" << member_name << endl);
		    tb = member_tb; // update tb for line/col tracking below
		    goto ns_resolved;
		}
		// resolve identifier: current namespace → global scope
		var = NULL;
		if ( !current_namespace.empty() )
		{
		    namespace_map_t::iterator nsi = namespace_map.find(current_namespace);
		    if ( nsi != namespace_map.end() )
		    {
			variable_map_iter vmi = nsi->second.find(ident_tb->str);
			if ( vmi != nsi->second.end() )
			    var = vmi->second;
		    }
		}
		if ( !var )
		    var = findVariable(ident_tb->str);
		// class method: resolve unqualified member name through __this
		if ( !var && code && code->method && code->method->owner_class )
		{
		    DataDefCLASS *cls = code->method->owner_class;
		    std::string mname = ident_tb->str;
		    ssize_t ofs = cls->m_offset(mname);
		    if ( ofs >= 0 )
		    {
			DataDef *mtype = cls->m_type(mname);
			// find __this parameter
			std::string thisid = "__this";
			Variable *thisvar = code->method->findParameter(thisid);
			if ( thisvar )
			{
			    Variable *member = new Variable(mname, *mtype, 1, NULL, false);
			    exStack.push(new TokenMember(*thisvar, *member, ofs));
			    break;
			}
		    }
		}
		// lazy-load check: symbol registered by #include but not yet created
		if ( !var )
		    var = lazy_resolve(ident_tb->str);
		if ( !var && peekToken() && peekToken()->id() == TokenID::tkOpBrk )
		{
		    // dlsym fallback: try to resolve as a libc/system function
		    std::string fname = ident_tb->str;
		    void *sym = dlsym(RTLD_DEFAULT, fname.c_str());
		    if ( sym )
		    {
			var = addFunction(fname,
			    datatype_vec_t{DataType::dtINT64},
			    (fVOIDFUNC)sym);
			DBG(if (var) cout << "parseExpression() dlsym fallback resolved " << fname << " at " << (uint64_t)sym << endl);
		    }
		}
		if ( !var )
		{
		    DBG(cerr << "parseExpression() failed to resolve identifier " << ident_tb->str << endl);
		    Throw(tb) << "use of undeclared identifier '" << ident_tb->str << '\'' << flush;
		}
		ns_resolved:
		if ( var->type->is_function() )
		{
		    // function pointer variable (DataDefFPTR) — different from regular functions
		    if ( var->type->is_numeric() )
		    {
			// FPTR variable: if followed by (, call through pointer
			if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
			{
			    TokenCallFunc *tc = new TokenCallFunc(*var);
			    tb = nextToken();
			    tc->line = tb->line;
			    tc->column = tb->column;
			    tb = parseCallFunc(tc);
			    opStack.push(tc);
			    if ( tb->id() == TokenID::tkSemi )
				done = true;
			}
			else
			{
			    // FPTR variable as value — push onto exStack
			    exStack.push(new TokenVar(*var));
			}
			break;
		    }
		    // Regular function identifier.
		    // C function-to-pointer decay: a bare function name used as an
		    // rvalue (RHS of assignment) becomes its address, so
		    // `fptr = func_name;` writes the function's address into fptr.
		    // Other contexts (e.g. `cout << endl;`, where BSL consumes a
		    // no-arg ostream-taking function) keep the pre-decay behavior.
		    {
			TokenBase *peek_after = peekToken();
			TokenID peek_id = peek_after ? peek_after->id() : TokenID::tkBase;
			bool followed_by_paren = (peek_id == TokenID::tkOpBrk);
			// Value-context followers: struct/array-init element end,
			// call-arg end, ternary branch separator - a bare function
			// name in these positions is passing/returning its address.
			bool followed_by_value_end =
			    peek_id == TokenID::tkComma || peek_id == TokenID::tkClBrk
			 || peek_id == TokenID::tkClSqr || peek_id == TokenID::tkClBrc
			 || peek_id == TokenID::tkTerC
			 // Binary comparison / logical / bitwise operators: a bare
			 // function name on either side of these is its address
			 // (function-to-pointer decay), not a call. Closes patterns
			 // like `t->fn == do_cast && tmp->...` where do_cast was
			 // previously pushed as a TokenCallFunc, then the operator
			 // was silently consumed and the next token mis-parsed.
			 || peek_id == TokenID::tkEquals || peek_id == TokenID::tkNotEq
			 || peek_id == TokenID::tkLT     || peek_id == TokenID::tkLE
			 || peek_id == TokenID::tkGT     || peek_id == TokenID::tkGE
			 || peek_id == TokenID::tkLand   || peek_id == TokenID::tkLor
			 || peek_id == TokenID::tkBand   || peek_id == TokenID::tkBor
			 || peek_id == TokenID::tkXor;
			bool in_assign_context = false;
			if ( !opStack.empty() )
			{
			    TokenID opid = opStack.top()->id();
			    if ( opid == TokenID::tkAssign
			      || opid == TokenID::tkAddEq || opid == TokenID::tkSubEq
			      || opid == TokenID::tkMulEq  || opid == TokenID::tkDivEq
			      || opid == TokenID::tkModEq  || opid == TokenID::tkXorEq
			      || opid == TokenID::tkBandEq || opid == TokenID::tkBorEq
			      || opid == TokenID::tkBSLEq  || opid == TokenID::tkBSREq )
				in_assign_context = true;
			}
			if ( !followed_by_paren && (in_assign_context || followed_by_value_end) )
			{
			    DBG(cout << "Pushing function address (decay): " << var->name << " onto exStack" << endl);
			    exStack.push(new TokenVar(*var));
			    break;
			}
		    }
		    TokenCallFunc *tc = new TokenCallFunc(*var);
		    tb = nextToken();
		    tc->line = tb->line;
		    tc->column = tb->column;
		    if ( tb->id() == TokenID::tkOpBrk )
		    {
			tb = parseCallFunc(tc);
			DBG(cout << "parseCallFunc returned with token " << (char)tb->get() << endl);
		    }
		    DBG(cout << "Pushing found function call: " << var->name << "() onto opStack" << endl);
		    opStack.push(tc);
		    if ( tb->id() == TokenID::tkSemi )
			done = true;
		    break;
		}
		if ( var->type->is_integer() )
		    DBG(cout << "Pushing found variable: " << var->name << '=' << (int)var->get<int>() << " onto exStack" << endl);
		else
		if ( var->type->is_real() )
		    DBG(cout << "Pushing found variable: " << var->name << '=' << (double)var->get<double>() << " onto exStack" << endl);
		else
		    DBG(cout << "Pushing found variable: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    }
	    case TokenType::ttVariable:
		var = &dynamic_cast<TokenVar *>(tb)->var;
		if ( var->type->is_integer() )
		    DBG(cout << "Pushing direct variable: " << var->name << '=' << (int)var->get<int>() << " onto exStack" << endl);
		else
		if ( var->type->is_real() )
		    DBG(cout << "Pushing direct variable: " << var->name << '=' << (double)var->get<double>() << " onto exStack" << endl);
		else
		    DBG(cout << "Pushing direct variable: " << var->name << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    case TokenType::ttFunction:
		Throw(tb) << "Got function!" << flush;
		break;
	    case TokenType::ttCallFunc:
		Throw(tb) << "Got call function!" << flush;
		break;
	    case TokenType::ttCallMethod:
		Throw(tb) << "Got call method!" << flush;
		break;
	    case TokenType::ttChar:
	        DBG(cout << "Pushing char: " << (int)tb->get() << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    default:
		DBG(std::cerr << "parseExpression() primary switch throwing token" << std::endl);
		Throw(tb) << "unexpected token type " << (int)opStack.top()->type() << flush;
	}
	if ( done ) { break; /* prevent eating next token */ }
	tb = peekToken();
	if ( tb->id() == TokenID::tkClBrk && !brackets )
	{
	    DBG(cout << "Hit ), no prior brackets, might be end of function?" << endl);
	    break;
	}
	if ( tb->id() == TokenID::tkClSqr )
	{
	    DBG(cout << "Hit ], end of subscript index" << endl);
	    break; // stop without consuming: let the subscript handler consume ]
	}
	if ( tb->id() == TokenID::tkClBrc )
	{
	    DBG(cout << "Hit }, end of expression (initializer or block terminator)" << endl);
	    break; // stop without consuming: caller handles }
	}
	// in conditional mode, stop at ; without consuming it
	// (needed for cast expressions: (TYPE *)expr; must not eat the ;)
	if ( conditional && tb->id() == TokenID::tkSemi )
	    break;
	// in conditional mode, stop at , without consuming — the caller
	// (parseCallFunc, for-init/incr, nested cast) uses the comma as an
	// argument / clause separator and needs it left in the stream.
	// Without this, `strcpy((char *)h + 8, "x")` lets the cast's inner
	// parseExpression eat the comma, and the outer parseExpression then
	// merges `"x"` into the first arg's expression.
	if ( conditional && tb->id() == TokenID::tkComma )
	    break;
	tb = nextToken();
    }

    if ( !opStack.empty() )
	DBG(cout << "Emptying operator stack" << endl);

    while ( !opStack.empty() )
	popOperator(opStack, exStack);

    DBG(cout << "parseExpression() exStack size: " << exStack.size() << endl);
    DBG(if ( !exStack.empty() ) std::cout << " exStack.top()->type() = " << (int)exStack.top()->type() << endl);

    DBG(std::cout << "Program::parseExpression() end" << std::endl);

    return exStack.empty() ? NULL : exStack.top();
}

// parse a structure definition
//
// forms:
// struct tag { type member; ... };
// struct { type member; } variable;
// struct tag { type member; } variable;
// struct tag variable;
// typedef struct tag alias;
// typedef struct tag { type member; } alias;
// typedef struct { type member; } alias;
// parse 'using' statement
// forms:
// using namespace std;       — import all members of std into global scope
// using std::cout;            — import single member
TokenBase *TokenUSING::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenUSING::parse() top" << std::endl);

    tn = pgm.nextToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'using'" << flush;

    // using namespace std;
    if ( tn->id() == TokenID::tkNAMESPACE )
    {
	tn = pgm.nextToken(); // namespace name
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting namespace name after 'using namespace'" << flush;
	std::string ns_name = ((TokenIdent *)tn)->str;
	namespace_map_t::iterator nsi = pgm.namespace_map.find(ns_name);
	if ( nsi == pgm.namespace_map.end() )
	    pgm.Throw(tn) << "Unknown namespace '" << ns_name << "'" << flush;
	// import all members into global scope
	for ( variable_map_iter vmi = nsi->second.begin(); vmi != nsi->second.end(); ++vmi )
	{
	    std::string name = vmi->first;
	    // only import if not already defined
	    if ( !pgm.findVariable(name) )
		pgm.tkProgram->variables.push_back(vmi->second);
	    DBG(std::cout << "TokenUSING::parse() imported " << ns_name << "::" << name << std::endl);
	}
	// expect semicolon
	tn = pgm.nextToken();
	if ( !tn || tn->id() != TokenID::tkSemi )
	    pgm.Throw(tn) << "Expecting ';' after using declaration" << flush;
	return NULL;
    }

    // using std::cout;
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string ns_name = ((TokenIdent *)tn)->str;
	tn = pgm.nextToken(); // should be ::
	if ( !tn || tn->id() != TokenID::tkNS )
	    pgm.Throw(tn) << "Expecting '::' in using declaration" << flush;
	tn = pgm.nextToken(); // member name
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting member name in using declaration" << flush;
	std::string member_name = ((TokenIdent *)tn)->str;
	namespace_map_t::iterator nsi = pgm.namespace_map.find(ns_name);
	if ( nsi == pgm.namespace_map.end() )
	    pgm.Throw(tn) << "Unknown namespace '" << ns_name << "'" << flush;
	variable_map_iter vmi = nsi->second.find(member_name);
	if ( vmi == nsi->second.end() )
	    pgm.Throw(tn) << "'" << member_name << "' is not a member of namespace '" << ns_name << "'" << flush;
	// import into global scope
	std::string name = member_name;
	if ( !pgm.findVariable(name) )
	    pgm.tkProgram->variables.push_back(vmi->second);
	// expect semicolon
	tn = pgm.nextToken();
	if ( !tn || tn->id() != TokenID::tkSemi )
	    pgm.Throw(tn) << "Expecting ';' after using declaration" << flush;
	return NULL;
    }

    pgm.Throw(tn) << "Unexpected token in using declaration" << flush;
    return NULL;
}


// parse a structure definition
//
// forms:
// struct tag { type member; ... };
// struct { type member; } variable;
// struct tag { type member; } variable;
// struct tag variable;
// typedef struct tag alias;
// typedef struct tag { type member; } alias;
// typedef struct { type member; } alias;
TokenBase *TokenSTRUCT::parse(Program &pgm)
{
    TokenIdent *tag = NULL;
    TokenBase *tn;
    TokenDataType *tdt;
    bool do_typedef = pgm.prevToken() ? pgm.prevToken()->id() == TokenID::tkTYPEDEF : false;
    datatype_map_iter bmi; // TokenDataType map
    datadef_map_iter dmi;  // DataDef map

    DBG(std::cout << std::endl << "TokenSTRUCT::parse() top" << std::endl);
    if ( !(tn=pgm.peekToken()) )
	pgm.Throw << "Unexpected end of input" << flush;

    // check for __attribute__((packed)) before or after tag
    bool is_packed = false;
    auto consume_attribute = [&]()
    {
	if ( tn && tn->type() == TokenType::ttIdentifier
	&&   ((TokenIdent *)tn)->str == "__attribute__" )
	{
	    pgm.nextToken(); // consume __attribute__
	    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
	    {
		pgm.nextToken(); // consume first (
		if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
		{
		    pgm.nextToken(); // consume second (
		    TokenBase *attr = pgm.nextToken(); // consume attribute name
		    if ( attr->type() == TokenType::ttIdentifier && ((TokenIdent *)attr)->str == "packed" )
			is_packed = true;
		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
			pgm.nextToken(); // consume first )
		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
			pgm.nextToken(); // consume second )
		}
	    }
	    tn = pgm.peekToken();
	}
    };

    // __attribute__ can appear before the tag name
    consume_attribute();

    // optional struct tag name
    if ( tn->type() == TokenType::ttIdentifier )
    {
	tag = (TokenIdent *)pgm.nextToken(); // consume tag
	DBG(cout << "TokenSTRUCT::parse() got tag " << tag->str << endl);
	tn = pgm.peekToken(); // peek at what follows the tag
	if ( !tn )
	    pgm.Throw << "Unexpected end of input after struct tag" << flush;
    }

    // __attribute__ can also appear after the tag name
    consume_attribute();

    // if no brace, then this structure type must already be defined
    // and we are either doing a typedef, or a variable declaration
    if ( tn->id() != TokenID::tkOpBrc )
    {
	if ( !tag )
	    pgm.Throw(tn) << "Expecting '{' or identifier after struct" << flush;
	dmi = pgm.struct_map.find(tag->str);

	// plain forward declaration: struct tag;
	if ( tn->id() == TokenID::tkSemi )
	{
	    if ( dmi == pgm.struct_map.end() )
	    {
		DataDefSTRUCT *fwd = new DataDefSTRUCT(tag->str, 0);
		pgm.struct_map[tag->str] = fwd;
	    }
	    pgm.nextToken(); // consume ';'
	    DBG(cout << "TokenSTRUCT::parse() forward declaration of struct " << tag->str << endl);
	    return NULL;
	}

	// forward typedef: typedef struct tag_name alias; (struct not yet defined)
	if ( dmi == pgm.struct_map.end() )
	{
	    // create placeholder struct (size 0, no members) for forward declaration
	    DataDefSTRUCT *fwd = new DataDefSTRUCT(tag->str, 0);
	    pgm.struct_map[tag->str] = fwd;
	    dmi = pgm.struct_map.find(tag->str);
	    DBG(cout << "TokenSTRUCT::parse() forward declaration of struct " << tag->str << endl);
	}
	// typedef struct tag alias
	if ( do_typedef )
	{
	    tn = pgm.nextToken(); // consume the alias identifier
	    if ( tn->type() != TokenType::ttIdentifier )
		pgm.Throw(tn) << "Expecting identifier after struct tag in typedef" << flush;
	    TokenIdent *alias = (TokenIdent *)tn;
	    if ( (bmi=pgm.datatype_map.find(alias->str)) != pgm.datatype_map.end() )
		pgm.Throw(tn) << "Identifier already defined" << flush;
	    tdt = new TokenDataType(alias->str.c_str(), *dmi->second);
	    pgm.datatype_map[alias->str] = tdt;
	    // also register tag in struct_map so "struct tag" works
	    pgm.struct_map[alias->str] = dmi->second;
	    return NULL;
	}

	// struct tag variable; — declare variable of existing struct type
	string tname("struct ");
	tname.append(tag->str);
	tdt = new TokenDataType(tname.c_str(), *dmi->second);
	return pgm.parseDeclaration(tdt);
    }

    // ---- defining a new structure: struct [tag] { type member; ... } ----

    pgm.nextToken(); // consume '{'

    DataDefSTRUCT *dds = new DataDefSTRUCT(tag ? tag->str : "anonymous", 0);
    if ( is_packed || pgm.pack_stack_top() == 1 )
	dds->pack = 1;
    else if ( pgm.pack_stack_top() > 0 )
	dds->pack = pgm.pack_stack_top();
    DBG(cout << "TokenSTRUCT::parse() defining struct " << dds->name << endl);

    // Pre-register the tag (if any) before parsing the body so fields like
    // `struct hashstr_data *next;` inside `struct hashstr_data { ... }` can
    // resolve the self-reference. The struct is treated as "incomplete" at
    // this point (size 0 members none); pointer-to-incomplete works because
    // DataDefPTR only needs an 8-byte pointer size.
    bool was_pre_registered = false;
    if ( tag && pgm.struct_map.find(tag->str) == pgm.struct_map.end() )
    {
	pgm.struct_map[tag->str] = dds;
	was_pre_registered = true;
	DBG(cout << "TokenSTRUCT::parse() pre-registered " << tag->str << " for self-reference" << endl);
    }

    while ( (tn=pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
    {
	while ( tn && (tn->id() == TokenID::tkCONST) )
	{
	    pgm.nextToken(); // consume qualifier
	    tn = pgm.peekToken();
	}

	// expect a data type token (or typedef'd identifier, or 'struct Tag')
	TokenDataType *mtype = NULL;
	if ( tn->type() == TokenType::ttDataType )
	    mtype = (TokenDataType *)pgm.nextToken();
	else if ( tn->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)tn)->str;
	    datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	    if ( tdmi != pgm.datatype_map.end() )
	    {
		pgm.nextToken(); // consume the identifier
		mtype = tdmi->second;
	    }
	    else
		pgm.Throw(tn) << "Expecting type in struct definition, got '" << tname << "'" << flush;
	}
	else if ( tn->id() == TokenID::tkSTRUCT )
	{
	    pgm.nextToken(); // consume 'struct'
	    TokenBase *stag = pgm.peekToken();
	    if ( stag && stag->id() == TokenID::tkOpBrc )
	    {
		// Support anonymous nested struct members like:
		//   struct { int x; char y[8]; } member;
		pgm.nextToken(); // consume '{'
		DataDefSTRUCT *inner = new DataDefSTRUCT("anonymous", 0);
		while ( (tn = pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
		{
		    TokenDataType *inner_type = NULL;
		    if ( tn->type() == TokenType::ttDataType )
			inner_type = (TokenDataType *)pgm.nextToken();
		    else if ( tn->type() == TokenType::ttIdentifier )
		    {
			std::string tname = ((TokenIdent *)tn)->str;
			datatype_map_iter tdmi = pgm.datatype_map.find(tname);
			if ( tdmi == pgm.datatype_map.end() )
			    pgm.Throw(tn) << "Expecting type in anonymous struct definition, got '" << tname << "'" << flush;
			pgm.nextToken();
			inner_type = tdmi->second;
		    }
		    else if ( tn->id() == TokenID::tkSTRUCT )
		    {
			pgm.nextToken();
			TokenBase *inner_tag = pgm.nextToken();
			if ( !inner_tag || inner_tag->type() != TokenType::ttIdentifier )
			    pgm.Throw(inner_tag ? inner_tag : tn) << "Expecting struct name" << flush;
			std::string sname = ((TokenIdent *)inner_tag)->str;
			datadef_map_iter sdmi = pgm.struct_map.find(sname);
			if ( sdmi == pgm.struct_map.end() )
			    pgm.Throw(inner_tag) << "Unknown struct type '" << sname << "'" << flush;
			inner_type = new TokenDataType(sname.c_str(), *sdmi->second);
		    }
		    else
			pgm.Throw(tn) << "Expecting type in anonymous struct definition" << flush;

		    DataDef *inner_member_dd = &inner_type->definition;
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
		    {
			pgm.nextToken();
			inner_member_dd = pgm.getPointerType(inner_member_dd);
		    }

		    tn = pgm.nextToken();
		    if ( !is_contextual_identifier_token(tn) )
			pgm.Throw(tn) << "Expecting member name in anonymous struct definition" << flush;
		    std::string inner_name = contextual_identifier_name(tn);

		    size_t inner_count = 1;
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
		    {
			pgm.nextToken();
			int64_t n = parse_constant_integer_expression(pgm);
			if ( n <= 0 )
			    pgm.Throw(tn) << "Fixed-size array dimension must be positive" << flush;
			TokenBase *cl = pgm.nextToken();
			if ( !cl || cl->id() != TokenID::tkClSqr )
			    pgm.Throw(cl ? cl : tn) << "Expected ']' in anonymous struct member array declaration" << flush;
			inner_count *= (size_t)n;
		    }

		    inner->addMember(inner_name, *inner_member_dd, inner_count);
		    tn = pgm.nextToken();
		    if ( !tn || tn->id() != TokenID::tkSemi )
			pgm.Throw(tn ? tn : stag) << "Expecting ';' after anonymous struct member" << flush;
		}
		if ( !tn || tn->id() != TokenID::tkClBrc )
		    pgm.Throw(tn ? tn : stag) << "Unexpected end of input in anonymous struct definition" << flush;
		pgm.nextToken(); // consume '}'
		inner->finalize();
		mtype = new TokenDataType("anonymous", *inner);
	    }
	    else
	    {
		stag = pgm.nextToken();
		if ( stag->type() != TokenType::ttIdentifier )
		    pgm.Throw(stag) << "Expecting struct name" << flush;
		std::string sname = ((TokenIdent *)stag)->str;
		datadef_map_iter sdmi = pgm.struct_map.find(sname);
		if ( sdmi == pgm.struct_map.end() )
		{
		    DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
		    pgm.struct_map[sname] = fwd;
		    sdmi = pgm.struct_map.find(sname);
		}
		mtype = new TokenDataType(sname.c_str(), *sdmi->second);
	    }
	}
	else
	    pgm.Throw(tn) << "Expecting type in struct definition" << flush;

		DataDef *base_member_dd = &mtype->definition;
		bool done_members = false;
		while ( !done_members )
		{
		    // Each declarator on the line can carry its own pointer stars:
		    // `int *a, b;` or `struct foo *next, *prev;`.
		    DataDef *member_dd = base_member_dd;
		    bool mem_fnptr_base = (dynamic_cast<DataDefFPTR *>(member_dd) != NULL);
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
		    {
			pgm.nextToken(); // consume '*'
			if ( !mem_fnptr_base )
			    member_dd = pgm.getPointerType(member_dd);
		    }

		    // expect member name
		    tn = pgm.nextToken();
		    if ( tn && tn->id() == TokenID::tkOpBrk )
		    {
			TokenBase *inner = pgm.nextToken();
			if ( inner && inner->id() == TokenID::tkStar )
			{
			    // Typed function-pointer member, e.g. `void (*callback)(void *)`
			    // or `char *(*resolver)(int)`.
			    tn = pgm.nextToken();
			    if ( !is_contextual_identifier_token(tn) )
				pgm.Throw(tn) << "Expecting member name in function pointer struct declarator" << flush;
			    std::string mname = contextual_identifier_name(tn);
			    tn = pgm.nextToken();
			    if ( !tn || tn->id() != TokenID::tkClBrk )
				pgm.Throw(tn ? tn : inner) << "Expected ')' after function pointer member name" << flush;
			    tn = pgm.nextToken();
			    if ( !tn || tn->id() != TokenID::tkOpBrk )
				pgm.Throw(tn ? tn : inner) << "Expected '(' after function pointer member name" << flush;
			    FuncDef *func = pgm.parseFnPtrParams(*member_dd);
			    DataDefFPTR *fptr_type = new DataDefFPTR(func);
			    member_dd = fptr_type;

			    dds->addMember(mname, *member_dd, 1);
			    DBG(cout << "TokenSTRUCT::parse() added function pointer member " << mname
				<< " (size " << member_dd->size << ", total " << dds->size << ')' << endl);

			    tn = pgm.nextToken();
			    if ( !tn )
				pgm.Throw(inner) << "Unexpected end of input after function pointer struct member" << flush;
			    if ( tn->id() == TokenID::tkComma )
				continue;
			    if ( tn->id() != TokenID::tkSemi )
				pgm.Throw(tn) << "Expecting ';' after function pointer struct member" << flush;
			    done_members = true;
			    continue;
			}
			pgm.Throw(inner ? inner : tn) << "Unsupported parenthesized member declarator in struct definition" << flush;
		    }
		    if ( !is_contextual_identifier_token(tn) )
			pgm.Throw(tn) << "Expecting member name in struct definition" << flush;
		    std::string mname = contextual_identifier_name(tn);

		    // Optional fixed-array dimensions: `char d_name[256];`, `int m[4][8];`.
		    // Multiply the dimensions into a single count so the member reserves
		    // N*sizeof(base) bytes inline. Access via `&obj.member` yields a pointer
		    // to the start of the inline buffer.
		    size_t member_count = 1;
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
		    {
			pgm.nextToken(); // consume '['
			int64_t n = parse_constant_integer_expression(pgm);
			if ( n <= 0 )
			    pgm.Throw(tn) << "Fixed-size array dimension must be positive" << flush;
			TokenBase *cl = pgm.nextToken();
			if ( !cl || cl->id() != TokenID::tkClSqr )
			    pgm.Throw(cl ? cl : tn) << "Expected ']' in struct member array declaration" << flush;
			member_count *= (size_t)n;
		    }

		    dds->addMember(mname, *member_dd, member_count);
		    DBG(cout << "TokenSTRUCT::parse() added member " << member_dd->name << ' ' << mname
			<< " (size " << member_dd->size << " x " << member_count
			<< ", total " << dds->size << ')' << endl);

		    tn = pgm.nextToken();
		    if ( !tn )
			pgm.Throw << "Unexpected end of input in struct definition" << flush;
		    if ( tn->id() == TokenID::tkComma )
			continue;
		    if ( tn->id() != TokenID::tkSemi )
			pgm.Throw(tn) << "Expecting ';' after struct member" << flush;
		    done_members = true;
		}
	    }

    if ( !tn )
	pgm.Throw << "Unexpected end of input in struct definition" << flush;
    pgm.nextToken(); // consume '}'
    dds->finalize(); // round up size to struct alignment

    // register the struct type
    if ( tag )
    {
	dmi = pgm.struct_map.find(tag->str);
	if ( dmi != pgm.struct_map.end() )
	{
	    DataDefSTRUCT *existing = static_cast<DataDefSTRUCT *>(dmi->second);
	    if ( was_pre_registered && existing == dds )
	    {
		// We registered `dds` ourselves before body-parsing to enable
		// self-reference; the entry is already correct, nothing to do.
		DBG(cout << "TokenSTRUCT::parse() finalized self-registered struct " << tag->str << " size=" << dds->size << endl);
	    }
	    else if ( existing->size == 0 && existing->members.empty() )
	    {
		// Complete a forward-declared struct in place.
		existing->members = dds->members;
		existing->member_counts = dds->member_counts;
		existing->size = dds->size;
		existing->pack = dds->pack;
		existing->max_align = dds->max_align;
		DBG(cout << "TokenSTRUCT::parse() completed forward-declared struct " << tag->str << " size=" << existing->size << endl);
		delete dds;
		dds = existing;
	    }
	    else
		pgm.Throw(tag) << "Struct '" << tag->str << "' already defined" << flush;
	}
	else
	{
	    pgm.struct_map[tag->str] = dds;
	    DBG(cout << "TokenSTRUCT::parse() registered struct " << tag->str << " size=" << dds->size << endl);
	}
    }

    // what follows the closing brace?
    tn = pgm.peekToken();

    // typedef struct [tag] { ... } alias;
    if ( do_typedef )
    {
	tn = pgm.nextToken();
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting alias name in typedef" << flush;
	TokenIdent *alias = (TokenIdent *)tn;
	if ( (bmi=pgm.datatype_map.find(alias->str)) != pgm.datatype_map.end() )
	    pgm.Throw(tn) << "Identifier '" << alias->str << "' already defined" << flush;
	tdt = new TokenDataType(alias->str.c_str(), *dds);
	pgm.datatype_map[alias->str] = tdt;
	// also register in struct_map so "struct alias" works
	pgm.struct_map[alias->str] = dds;
	DBG(cout << "TokenSTRUCT::parse() typedef alias " << alias->str << endl);
	return NULL;
    }

    // struct [tag] { ... } variable;
    // struct [tag] { ... } *first_whogr, *last_whogr;  (pointer decl)
    if ( tn && (tn->type() == TokenType::ttIdentifier
	     || tn->id() == TokenID::tkMul) )
    {
	string tname("struct ");
	tname.append(tag ? tag->str : "anonymous");
	tdt = new TokenDataType(tname.c_str(), *dds);
	return pgm.parseDeclaration(tdt);
    }

    // struct tag { ... }; — just a type definition, nothing to compile
    if ( tn && tn->id() == TokenID::tkSemi )
    {
	pgm.nextToken(); // consume ';'
	return NULL;
    }

    pgm.Throw(tn) << "Expecting variable name or ';' after struct definition" << flush;
    return NULL;
}

// parse a class definition
// forms:
// class Name { type member; rettype method() { ... } };
// class Name variable;
// typedef class Name alias;
TokenBase *TokenCLASS::parse(Program &pgm)
{
    TokenIdent *tag = NULL;
    TokenBase *tn;
    TokenDataType *tdt;
    bool do_typedef = pgm.prevToken() ? pgm.prevToken()->id() == TokenID::tkTYPEDEF : false;
    datatype_map_iter bmi;
    datadef_map_iter dmi;

    DBG(std::cout << std::endl << "TokenCLASS::parse() top" << std::endl);
    if ( !(tn=pgm.peekToken()) )
	pgm.Throw << "Unexpected end of input" << flush;

    // class name is required (no anonymous classes)
    if ( tn->type() == TokenType::ttIdentifier )
    {
	tag = (TokenIdent *)pgm.nextToken();
	DBG(cout << "TokenCLASS::parse() got name " << tag->str << endl);
	tn = pgm.peekToken();
	if ( !tn )
	    pgm.Throw << "Unexpected end of input after class name" << flush;
    }

    // if no brace, class type must already be defined
    if ( tn->id() != TokenID::tkOpBrc )
    {
	if ( !tag )
	    pgm.Throw(tn) << "Expecting class name" << flush;
	if ( (dmi=pgm.struct_map.find(tag->str)) == pgm.struct_map.end() )
	    pgm.Throw(tn) << "Unknown class type '" << tag->str << "'" << flush;
	if ( do_typedef )
	{
	    tn = pgm.nextToken();
	    if ( tn->type() != TokenType::ttIdentifier )
		pgm.Throw(tn) << "Expecting identifier in typedef" << flush;
	    TokenIdent *alias = (TokenIdent *)tn;
	    if ( (bmi=pgm.datatype_map.find(alias->str)) != pgm.datatype_map.end() )
		pgm.Throw(tn) << "Identifier already defined" << flush;
	    tdt = new TokenDataType(alias->str.c_str(), *dmi->second);
	    pgm.datatype_map[alias->str] = tdt;
	    return NULL;
	}
	tdt = new TokenDataType(tag->str.c_str(), *dmi->second);
	return pgm.parseDeclaration(tdt);
    }

    // ---- defining a new class: class Name { ... } ----

    if ( !tag )
	pgm.Throw(tn) << "Class definition requires a name" << flush;

    pgm.nextToken(); // consume '{'

    DataDefCLASS *ddc = new DataDefCLASS(tag->str, 0, DataType::dtRESERVED);
    DBG(cout << "TokenCLASS::parse() defining class " << tag->str << endl);

    while ( (tn=pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
    {
	// expect a data type token
	if ( tn->type() != TokenType::ttDataType )
	    pgm.Throw(tn) << "Expecting type in class definition" << flush;
	TokenDataType *mtype = (TokenDataType *)pgm.nextToken();

	// check for pointer declarator(s): type * [*...] member_name
	DataDef *cmember_dd = &mtype->definition;
	while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
	{
	    pgm.nextToken(); // consume '*'
	    cmember_dd = pgm.getPointerType(cmember_dd);
	}

	// expect member name
	tn = pgm.nextToken();
	if ( tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting member name in class definition" << flush;
	std::string mname = ((TokenIdent *)tn)->str;

	// peek: is this a method (followed by '(') or a data member (followed by ';')?
	tn = pgm.peekToken();
	if ( tn && tn->id() == TokenID::tkOpBrk )
	{
	    pgm.nextToken(); // consume '('
	    // method declaration — parse as function, add to class methods
	    DBG(cout << "TokenCLASS::parse() parsing method " << mname << endl);
	    // mangle method name to avoid collisions: ClassName__methodName
	    std::string mangled = tag->str + "__" + mname;
	    pgm.parseFunction(*cmember_dd, mangled, ddc);
	    // find the variable that parseFunction created and add to class methods
	    Variable *mvar;
	    if ( (mvar=pgm.tkProgram->findVariable(mangled)) )
	    {
		ddc->methods.push_back(mvar);
		// also register under the unmangled name for method lookup
		ddc->method_map[mname] = mvar;
	    }
	}
	else
	{
	    // data member
	    ddc->addMember(mname, *cmember_dd, 1);
	    DBG(cout << "TokenCLASS::parse() added member " << cmember_dd->name << ' ' << mname
		<< " (size " << cmember_dd->size << ", total " << ddc->size << ')' << endl);
	    tn = pgm.nextToken();
	    if ( tn->id() != TokenID::tkSemi )
		pgm.Throw(tn) << "Expecting ';' after class member" << flush;
	}
    }

    if ( !tn )
	pgm.Throw << "Unexpected end of input in class definition" << flush;
    pgm.nextToken(); // consume '}'

    // register the class type
    if ( pgm.struct_map.find(tag->str) != pgm.struct_map.end() )
	pgm.Throw(tag) << "Class '" << tag->str << "' already defined" << flush;
    pgm.struct_map[tag->str] = ddc;
    // also register as a data type so "ClassName var;" works without "class" prefix
    tdt = new TokenDataType(tag->str.c_str(), *ddc);
    pgm.datatype_map[tag->str] = tdt;
    DBG(cout << "TokenCLASS::parse() registered class " << tag->str << " size=" << ddc->size << endl);

    // what follows?
    tn = pgm.peekToken();

    if ( do_typedef )
    {
	tn = pgm.nextToken();
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting alias name in typedef" << flush;
	TokenIdent *alias = (TokenIdent *)tn;
	tdt = new TokenDataType(alias->str.c_str(), *ddc);
	pgm.datatype_map[alias->str] = tdt;
	pgm.struct_map[alias->str] = ddc;
	return NULL;
    }

    // class Name { ... } variable;
    if ( tn && tn->type() == TokenType::ttIdentifier )
    {
	tdt = new TokenDataType(tag->str.c_str(), *ddc);
	return pgm.parseDeclaration(tdt);
    }

    // class Name { ... }; — just a definition
    if ( tn && tn->id() == TokenID::tkSemi )
    {
	pgm.nextToken();
	return NULL;
    }

    pgm.Throw(tn) << "Expecting variable name or ';' after class definition" << flush;
    return NULL;
}

TokenBase *TokenGOTO::parse(Program &pgm)
{
    DBG(std::cout << std::endl << "TokenGOTO::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( !tn || !is_contextual_identifier_token(tn) )
	pgm.Throw(tn ? tn : (TokenBase *)this) << "expected label name after 'goto'" << flush;
    target = contextual_identifier_name(tn);
    TokenBase *semi = pgm.nextToken();
    if ( !semi || semi->id() != TokenID::tkSemi )
	pgm.Throw(semi ? semi : tn) << "expected ';' after 'goto " << target << "'" << flush;
    return this;
}

TokenBase *TokenRETURN::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenRETURN::parse()" << std::endl);
    tn = pgm.nextToken();

    // return with no value
    if ( tn->id() == TokenID::tkSemi )
	return this;

    // parse first return expression
    returns = pgm.parseExpression(tn);

    // Multi-return detection: parseExpression consumed either `,` or `;`
    // to stop the first return expression. The cleanest signal is the
    // consumed stop token itself (curToken()): only `,` indicates that
    // a second expression follows. Without that check, a plain
    // `return x;` followed by any identifier-starting statement (e.g.
    // `noop(ch);`) misfires as multi-return because the peek-based
    // heuristic can't distinguish a second return expression from the
    // start of the next statement.
    auto looks_like_second_return = [&]() -> bool {
	TokenBase *stop = pgm.curToken();
	if ( !stop || stop->id() != TokenID::tkComma )
	    return false;
	TokenBase *p = pgm.peekToken();
	if ( !p ) return false;
	if ( p->id() == TokenID::tkSemi ) return false;
	if ( p->id() == TokenID::tkClBrc ) return false;
	if ( p->type() == TokenType::ttSymbol ) return false;
	if ( p->type() == TokenType::ttKeyword ) return false;
	if ( p->type() == TokenType::ttDataType ) return false;
	return true;
    };
    if ( looks_like_second_return() )
    {
	return_exprs.push_back(returns);
	tn = pgm.nextToken();
	return_exprs.push_back(pgm.parseExpression(tn));
	while ( looks_like_second_return() )
	{
	    tn = pgm.nextToken();
	    return_exprs.push_back(pgm.parseExpression(tn));
	}
	returns = NULL; // multi-return uses return_exprs instead

	// mark the enclosing function as multi-return so it gets __retbuf at compile time
	TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
	if ( code && code->method )
	{
	    FuncDef *fdef = (FuncDef *)code->method->returns.type;
	    if ( fdef && fdef->return_types.empty() )
	    {
		// populate return_types with int64 for each value (inferred)
		for ( size_t i = 0; i < return_exprs.size(); ++i )
		    fdef->return_types.push_back(&ddINT64);
	    }
	}
	DBG(std::cout << "TokenRETURN::parse() multi-return with " << return_exprs.size() << " values" << std::endl);
    }

    return this;
}

TokenBase *TokenIF::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenIF::parse()" << std::endl);

    DBG(cout << "TokenIF::parse() calling parse_parenthesized_expression()" << endl);
    if ( !(condition = parse_parenthesized_expression(pgm, "if", true)) )
	pgm.Throw << "Failed to parse if expression" << flush;

    tn = pgm.nextToken();
    DBG(cout << "TokenIF::parse() calling statement=parseStatement(" << (char)tn->get() << ')' << endl);
    if ( !(statement=pgm.parseStatement(tn)) )
	pgm.Throw(tn) << "Failed to parse if statement" << flush;

    tn = pgm.peekToken();
    if ( tn && tn->id() == TokenID::tkELSE )
    {
	tn = pgm.nextToken(); // get the else
	tn = pgm.nextToken(); // skip the else
	DBG(cout << "TokenIF::parse() calling elsestmt=parseStatement(" << (char)tn->get() << ')' << endl);
	elsestmt = pgm.parseStatement(tn);
	if ( !elsestmt )
	    pgm.Throw(tn) << "parse error on else" << flush;
    }
    else
    if ( tn )
	DBG(cout << "TokenIF::peekToken() type: " << (int)tn->type() << " id: " << (int)tn->id() << ')' << endl);

    return this;
}

TokenBase *TokenFOR::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenFOR::parse() START" << std::endl);
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenFOR::parse() expecting (" << endl);
	pgm.Throw(tn) << "expecting ( after for" << flush;
    }

    tn = pgm.nextToken();

    // detect range-based for: for (type var : container)
    bool typed_for_init = false;
    if ( tn->type() == TokenType::ttDataType )
    {
	TokenDataType *dt = (TokenDataType *)tn;
	TokenBase *tn2 = pgm.nextToken();
	if ( tn2->type() == TokenType::ttIdentifier )
	{
	    TokenBase *tn3 = pgm.peekToken();
	    if ( tn3 && tn3->id() == TokenID::tkTerC )
	    {
		pgm.nextToken(); // consume the colon

		TokenFOREACH *fe = new TokenFOREACH();
		fe->file = this->file;
		fe->line = this->line;
		fe->column = this->column;
		fe->elemtype = &dt->definition;
		fe->elemname = ((TokenIdent *)tn2)->str;

		DBG(cout << "TokenFOR::parse() range-for detected: " << dt->definition.name << ' ' << fe->elemname << endl);

		// add the loop variable to the current scope
		TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
		fe->elemvar = pgm.addVariable(code, dt->definition, fe->elemname, 1, NULL, false);

		// parse the container expression
		TokenBase *tn4 = pgm.nextToken();
		fe->container = pgm.parseExpression(tn4, true);
		if ( !fe->container )
		    pgm.Throw(tn4) << "Failed to parse container expression in range-for" << flush;

		tn4 = pgm.nextToken();
		if ( tn4->id() != TokenID::tkClBrk )
		    pgm.Throw(tn4) << "Expecting ) after range-for container" << flush;

		tn4 = pgm.nextToken();
		fe->statement = pgm.parseStatement(tn4);
		if ( !fe->statement )
		    pgm.Throw(tn4) << "Failed to parse range-for body" << flush;

		DBG(std::cout << "TokenFOR::parse() range-for END" << std::endl);
		return fe;
	    }
	}

	// not range-for — traditional for with type declaration
	DBG(cout << "TokenFOR::parse() traditional for with type declaration" << endl);
	pgm.pushToken(tn2);
	initialize = pgm.parseDeclaration(dt);
	typed_for_init = true;
    }
    else
    {
	if ( tn->id() == TokenID::tkSemi )
	{
	    initialize = NULL;
	}
	else
	{
	    DBG(cout << "TokenFOR::parse() initialize: calling parseExpression(" << (char)tn->get() << ')' << endl);
	    // conditional=true so parseExpression stops on `;` WITHOUT consuming;
	    // comma continuations read below get to see `,` left in the stream.
	    if ( !(initialize = pgm.parseExpression(tn, true)) )
		pgm.Throw(tn) << "Failed to parse initialize" << flush;
	}
    }

    // C comma-expression init: `for (a=0, b=1, ... ; ...)`. parseExpression
    // consumes `,`, so after a comma-terminated init the peek is already
    // the next expression starter; accept either case.
    while ( tn->id() != TokenID::tkSemi && pgm.peekToken() )
    {
	TokenBase *pk = pgm.peekToken();
	if ( pk->id() == TokenID::tkComma ) { pgm.nextToken(); pk = pgm.peekToken(); }
	if ( !pk || pk->id() == TokenID::tkSemi ) break;
	if ( typed_for_init && pk->type() == TokenType::ttDataType )
	{
	    tn = pgm.nextToken();
	    TokenBase *extra = pgm.parseStatement(tn);
	    if ( extra ) init_extras.push_back(extra);
	    continue;
	}
	// anything else must look like an expression starter
	if ( pk->type() == TokenType::ttSymbol || pk->type() == TokenType::ttKeyword
	  || pk->type() == TokenType::ttDataType ) break;
	tn = pgm.nextToken();
	TokenBase *extra = pgm.parseExpression(tn, true);
	if ( extra ) init_extras.push_back(extra);
    }

    if ( tn->id() != TokenID::tkSemi )
	tn = pgm.nextToken(); // consume `;` after init when init wasn't empty
    if ( tn->id() != TokenID::tkSemi )
	pgm.Throw(tn) << "Expecting ';' after for init" << flush;

    tn = pgm.nextToken();
    if ( tn->id() == TokenID::tkSemi )
	condition = new TokenInt(1);
    else
    {
	DBG(cout << "TokenFOR::parse() condition: calling parseExpression(" << (char)tn->get() << ')' << endl);
	if ( !(condition = pgm.parseExpression(tn, true)) )
	    pgm.Throw(tn) << "Failed to parse expression" << flush;
	tn = pgm.nextToken();  // consume ; separator between condition and increment
    }
    if ( tn->id() != TokenID::tkSemi )
	pgm.Throw(tn) << "Expecting ';' after for condition" << flush;
    tn = pgm.nextToken();  // first token of increment expression
    if ( tn->id() == TokenID::tkClBrk )
	increment = NULL;
    else
    {
	DBG(cout << "TokenFOR::parse() increment: calling parseExpression(" << (char)tn->get() << ')' << endl);
	if ( !(increment = pgm.parseExpression(tn, true)) )
	    pgm.Throw(tn) << "Failed to parse increment" << flush;
    }

    // C comma-expression increment: `for (...; ...; i++, j--, k++)`.
    while ( increment && pgm.peekToken() )
    {
	TokenBase *pk = pgm.peekToken();
	if ( pk->id() == TokenID::tkComma ) { pgm.nextToken(); pk = pgm.peekToken(); }
	if ( !pk || pk->id() == TokenID::tkClBrk ) break;
	if ( pk->type() == TokenType::ttSymbol || pk->type() == TokenType::ttKeyword
	  || pk->type() == TokenType::ttDataType ) break;
	tn = pgm.nextToken();
	TokenBase *extra = pgm.parseExpression(tn, true);
	if ( extra ) incr_extras.push_back(extra);
    }

    if ( tn->id() != TokenID::tkClBrk )
	tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkClBrk )
	pgm.Throw(tn) << "Expecting )" << flush;

	    tn = pgm.nextToken();
	    pgm.resetPrevToken();
	    DBG(cout << "TokenFOR::parse() statement(s): calling parseStatement(" << (char)tn->get() << ')' << endl);
	    if ( !(statement = pgm.parseStatement(tn)) )
		pgm.Throw(tn) << "Failed to parse statement" << flush;

    DBG(std::cout << "TokenFOR::parse() END" << std::endl);

    return this;
}

TokenBase *TokenWHILE::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenWHILE::parse()" << std::endl);
    DBG(cout << "TokenWHILE::parse() calling parse_parenthesized_expression()" << endl);
    condition = parse_parenthesized_expression(pgm, "while", true);

    tn = pgm.nextToken();
    DBG(cout << "TokenWHILE::parse() calling parseStatement(" << (char)tn->get() << ')' << endl);
    statement = pgm.parseStatement(tn);

    return this;
}

TokenBase *TokenDO::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenDO::parse()" << std::endl);
    tn = pgm.nextToken();
    DBG(cout << "TokenDO::parse() calling parseStatement(" << (char)tn->get() << ')' << endl);
    statement = pgm.parseStatement(tn);

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkWHILE )
    {
	DBG(cerr << "TokenDO::parse() expecting while " << endl);
	pgm.Throw(tn) << "Expecting while after do" << flush;
    }
    DBG(cout << "TokenDO::parse() calling parse_parenthesized_expression()" << endl);
    condition = parse_parenthesized_expression(pgm, "do/while", true);

    return this;
}

// parse operator overload
TokenBase *TokenOPEROVER::parse(Program &pgm)
{
    TokenBase *tn;
    DBG(std::cout << std::endl << "TokenOPEROVER::parse()" << std::endl);
    tn = pgm.nextToken();
    // overload type conversion
    if ( tn->type() == TokenType::ttDataType )
    {
    }
    // overloading operator
    switch(tn->id())
    {
	// multi-token
	case TokenID::tkOpBrk:
	    if ( pgm.peekToken()->id() != TokenID::tkClBrk )
		pgm.Throw(pgm.peekToken()) << "Expecting )" << flush;
	    delete tn;
	    delete pgm.nextToken();
	    str = "()";
	    return this;
	case TokenID::tkOpSqr:
	    if ( pgm.peekToken()->id() != TokenID::tkClSqr )
		pgm.Throw(pgm.peekToken()) << "Expecting ]" << flush;
	    delete tn;
	    delete pgm.nextToken();
	    str = "[]";
	    return this;
	// MultiOp
	case TokenID::tkInc:
	case TokenID::tkDec:
	    str = ((TokenMultiOp *)tn)->str;
	    delete tn;
	    return this;

	// single character
	case TokenID::tkLT:
	case TokenID::tkGT:
	case TokenID::tkAdd:
	case TokenID::tkSub:
	case TokenID::tkMul:
	case TokenID::tkDiv:
	case TokenID::tkMod:
	case TokenID::tkBor:
	case TokenID::tkXor:
	case TokenID::tkBand:
	case TokenID::tkLnot:
	case TokenID::tkBnot:
	case TokenID::tkAssign:
	    str = tn->get();
	    delete tn;
	    return this;
	default:
	    pgm.Throw(tn) << "unexpected token type " << (int)tn->type() << flush;
    }
    return this;
}


TokenBase *TokenREGISTER::parse(Program &pgm)
{
    DBG(std::cout << "TokenREGISTER::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
        pgm.Throw << "Unexpected end of input after 'register'" << flush;

    // Accept `register struct ...`, `register TypedefName ...`, and
    // `register <primitive type> ...`. `register` is a C hint — we set
    // vfREGISTER for the primitive path where the Variable is numeric;
    // for struct/typedef paths the flag is dropped (the variable will
    // still live in a register when it's a pointer, which is typical).
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    TokenBase *decl = pgm.parseDeclaration(tdmi->second);
	    if ( decl && decl->type() == TokenType::ttDeclare )
		dynamic_cast<TokenDecl *>(decl)->var.flags |= vfREGISTER;
	    return decl;
	}
    }
    if ( tn->type() != TokenType::ttDataType )
        pgm.Throw(tn) << "Expecting type after 'register'" << flush;
    tn = pgm.nextToken();
    TokenBase *decl = pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
    if ( decl && decl->type() == TokenType::ttDeclare )
        dynamic_cast<TokenDecl *>(decl)->var.flags |= vfREGISTER;
    return decl;
}

static bool is_contextual_identifier_token(TokenBase *tb)
{
    if ( !tb )
	return false;
    if ( tb->type() == TokenType::ttIdentifier )
	return true;
    if ( tb->type() == TokenType::ttDataType )
    {
	TokenDataType *td = static_cast<TokenDataType *>(tb);
	if ( &td->definition == &ddSTRING )
	    return true;
    }
    if ( tb->type() != TokenType::ttKeyword )
	return false;
    switch ( tb->id() )
    {
	case TokenID::tkCLASS:
	case TokenID::tkVECTOR:
	case TokenID::tkMAP:
	case TokenID::tkSET:
	case TokenID::tkLIST:
	// C++ keywords that are valid C identifiers — `int try;`, struct
	// member named `new`, `void *catch_block`, etc.
	case TokenID::tkTRY:
	case TokenID::tkCATCH:
	case TokenID::tkTHROW:
	    return true;
	default:
	    return false;
    }
}

static std::string contextual_identifier_name(TokenBase *tb)
{
    if ( !tb )
	return "";
    if ( tb->type() == TokenType::ttIdentifier )
	return ((TokenIdent *)tb)->str;
    if ( tb->type() == TokenType::ttDataType )
    {
	TokenDataType *td = static_cast<TokenDataType *>(tb);
	if ( &td->definition == &ddSTRING )
	    return td->str;
    }
    if ( tb->id() == TokenID::tkCLASS
	|| tb->id() == TokenID::tkVECTOR
	|| tb->id() == TokenID::tkMAP
	|| tb->id() == TokenID::tkSET
	|| tb->id() == TokenID::tkLIST
	|| tb->id() == TokenID::tkTRY
	|| tb->id() == TokenID::tkCATCH
	|| tb->id() == TokenID::tkTHROW )
	return ((TokenKeyword *)tb)->str;
    return "";
}

// typedef TYPE alias; or typedef struct/class ... (struct/class detected via prevToken)
TokenBase *TokenTYPEDEF::parse(Program &pgm)
{
    DBG(std::cout << "TokenTYPEDEF::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'typedef'" << flush;

    // typedef struct ... or typedef class ... — handled by struct/class parse via prevToken
    if ( tn->id() == TokenID::tkSTRUCT || tn->id() == TokenID::tkCLASS )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->id() == TokenID::tkENUM )
    {
	pgm.nextToken(); // consume enum

	// optional tag name: typedef enum Tag { ... } Alias;
	tn = pgm.peekToken();
	if ( tn && tn->type() == TokenType::ttIdentifier )
	    pgm.nextToken();

	// optional body: typedef enum { ... } Alias;
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrc )
	{
	    TokenENUM tenum;
	    tenum.parse(pgm);
	}

	tn = pgm.nextToken();
	if ( !tn )
	    pgm.Throw << "Unexpected end of input in typedef enum" << flush;

	std::string alias;
	if ( tn->type() == TokenType::ttIdentifier )
	    alias = ((TokenIdent *)tn)->str;
	else if ( tn->type() == TokenType::ttDataType )
	    alias = ((TokenDataType *)tn)->str;
	else if ( tn->type() == TokenType::ttKeyword )
	    alias = ((TokenKeyword *)tn)->str;
	else
	    pgm.Throw(tn) << "Expecting alias name in typedef enum" << flush;

	TokenDataType *tdt = new TokenDataType(alias.c_str(), ddINT);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() enum alias " << alias << " = int" << std::endl);

	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return NULL;
    }

    // typedef TYPE alias; — primitive type alias
    DataDef *base_dd = NULL;
    if ( tn->type() == TokenType::ttDataType )
    {
	base_dd = &((TokenDataType *)pgm.nextToken())->definition;
    }
    else if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    base_dd = &tdmi->second->definition;
	}
    }
    if ( !base_dd )
	pgm.Throw(tn) << "Expecting type after 'typedef'" << flush;

    // handle pointer: typedef int *intptr;
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
    {
	pgm.nextToken();
	base_dd = pgm.getPointerType(base_dd);
    }

    // Function-pointer typedef Form 2: typedef RET (*NAME)(params);
    TokenBase *peek = pgm.peekToken();
    if ( peek && peek->id() == TokenID::tkOpBrk )
    {
	pgm.nextToken(); // consume '('
	TokenBase *star = pgm.nextToken();
	if ( !star || star->id() != TokenID::tkMul )
	    pgm.Throw(star ? star : tn) << "Expecting '*' in function pointer typedef" << flush;
	TokenBase *name_tok = pgm.nextToken();
	if ( !name_tok || name_tok->type() != TokenType::ttIdentifier )
	    pgm.Throw(name_tok ? name_tok : tn) << "Expecting identifier in function pointer typedef" << flush;
	std::string alias = ((TokenIdent *)name_tok)->str;
	TokenBase *rbrk = pgm.nextToken();
	if ( !rbrk || rbrk->id() != TokenID::tkClBrk )
	    pgm.Throw(rbrk ? rbrk : tn) << "Expecting ')' after function pointer name" << flush;
	TokenBase *open = pgm.nextToken();
	if ( !open || open->id() != TokenID::tkOpBrk )
	    pgm.Throw(open ? open : tn) << "Expecting '(' for parameter list" << flush;
	FuncDef *func = pgm.parseFnPtrParams(*base_dd);
	DataDefFPTR *fptr = new DataDefFPTR(func);
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *fptr);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() fptr (form 2): " << alias << std::endl);
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return NULL;
    }

    // get alias name (may be an identifier or an existing type name being redefined)
    tn = pgm.nextToken();
    std::string alias;
    if ( tn->type() == TokenType::ttIdentifier )
	alias = ((TokenIdent *)tn)->str;
    else if ( tn->type() == TokenType::ttDataType )
	alias = ((TokenDataType *)tn)->str;
    else if ( tn->type() == TokenType::ttKeyword )
	alias = ((TokenKeyword *)tn)->str;
    else
	pgm.Throw(tn) << "Expecting alias name in typedef" << flush;

    // Function-pointer typedef Form 1: typedef RET NAME(params);
    TokenBase *post = pgm.peekToken();
    if ( post && post->id() == TokenID::tkOpBrk )
    {
	pgm.nextToken(); // consume '('
	FuncDef *func = pgm.parseFnPtrParams(*base_dd);
	DataDefFPTR *fptr = new DataDefFPTR(func);
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *fptr);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() fptr (form 1): " << alias << std::endl);
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return NULL;
    }

    // register in datatype_map
    TokenDataType *tdt = new TokenDataType(alias.c_str(), *base_dd);
    pgm.datatype_map[alias] = tdt;
    DBG(std::cout << "TokenTYPEDEF::parse() " << alias << " = " << base_dd->name << std::endl);

    // consume semicolon
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	pgm.nextToken();

    return NULL;
}

// Parse a function-pointer parameter list. The opening '(' has already been
// consumed by the caller. Stops after consuming the closing ')'. Parameter
// names are optional and discarded — typedef signatures don't bind names.
FuncDef *Program::parseFnPtrParams(DataDef &returns)
{
    FuncDef *func = new FuncDef(returns);
    TokenBase *nt = nextToken();
    if ( !nt )
	Throw << "Unexpected end of input in function pointer typedef" << flush;

    // empty param list: ()
    if ( nt->id() == TokenID::tkClBrk )
	return func;

    // (void) as sole parameter = no parameters
    if ( nt->type() == TokenType::ttDataType
      && &((TokenDataType *)nt)->definition == &ddVOID
      && peekToken() && peekToken()->id() == TokenID::tkClBrk )
    {
	nextToken(); // consume ')'
	return func;
    }

    while ( nt )
    {
	while ( nt && nt->id() == TokenID::tkCONST )
	    nt = nextToken();

	// variadic
	if ( nt->id() == TokenID::tkDot )
	{
	    TokenBase *d2 = nextToken();
	    TokenBase *d3 = nextToken();
	    if ( !d2 || d2->id() != TokenID::tkDot || !d3 || d3->id() != TokenID::tkDot )
		Throw(nt) << "Expecting '...' for variadic parameter" << flush;
	    func->is_varargs = true;
	    func->parameters.push_back(&ddINT64);
	    nt = nextToken();
	    if ( !nt || nt->id() != TokenID::tkClBrk )
		Throw(nt) << "Expecting ')' after '...'" << flush;
	    return func;
	}

	// Resolve parameter type: plain type, struct Tag, or typedef alias
	DataDef *param_dd = NULL;
	if ( nt->id() == TokenID::tkSTRUCT )
	{
	    TokenBase *tag = nextToken();
	    if ( !tag || tag->type() != TokenType::ttIdentifier )
		Throw(tag ? tag : nt) << "Expecting struct name after 'struct'" << flush;
	    std::string sname = ((TokenIdent *)tag)->str;
	    datadef_map_iter sdmi = struct_map.find(sname);
	    if ( sdmi == struct_map.end() )
	    {
		DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
		struct_map[sname] = fwd;
		sdmi = struct_map.find(sname);
	    }
	    param_dd = sdmi->second;
	}
	else if ( nt->type() == TokenType::ttDataType )
	{
	    param_dd = &((TokenDataType *)nt)->definition;
	}
	else if ( nt->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)nt)->str;
	    datatype_map_iter tdmi = datatype_map.find(tname);
	    if ( tdmi == datatype_map.end() )
		Throw(nt) << "Unknown type '" << tname << "' in function pointer typedef" << flush;
	    param_dd = &tdmi->second->definition;
	}
	else
	{
	    Throw(nt) << "Expecting parameter type in function pointer typedef" << flush;
	}

	// Pointer decorators
	while ( peekToken() && peekToken()->id() == TokenID::tkMul )
	{
	    nextToken();
	    param_dd = getPointerType(param_dd);
	}

	while ( peekToken() && peekToken()->id() == TokenID::tkCONST )
	    nextToken();

	// Optional parameter name (discard)
	if ( peekToken() && is_contextual_identifier_token(peekToken()) )
	    nextToken();

	func->parameters.push_back(param_dd);

	// Next: ',' or ')'
	nt = nextToken();
	if ( !nt )
	    Throw << "Unexpected end of input in function pointer typedef" << flush;
	if ( nt->id() == TokenID::tkClBrk )
	    return func;
	if ( nt->id() != TokenID::tkComma )
	    Throw(nt) << "Expecting ',' or ')' in function pointer typedef" << flush;
	nt = nextToken(); // next parameter
    }

    Throw << "Unexpected end of input in function pointer typedef" << flush;
    return NULL; // unreachable
}

// parse enum { NAME, NAME = val, ... } [;]
// registers each enumerator as a #define constant
TokenBase *TokenENUM::parse(Program &pgm)
{
    DBG(std::cout << "TokenENUM::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();

    // optional tag name: enum colors { ... }
    if ( tn && tn->type() == TokenType::ttIdentifier )
	pgm.nextToken(); // consume tag name (ignored for now)

    tn = pgm.peekToken();
    if ( !tn || tn->id() != TokenID::tkOpBrc )
	pgm.Throw(tn) << "Expecting '{' after enum" << flush;
    pgm.nextToken(); // consume '{'

    int64_t val = 0;
    while ( (tn = pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
    {
	if ( tn->id() == TokenID::tkComma ) { pgm.nextToken(); continue; }
	if ( !is_contextual_identifier_token(tn) )
	    pgm.Throw(tn) << "Expecting identifier in enum" << flush;
	std::string name = contextual_identifier_name(pgm.nextToken());

	// check for = explicit value
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkAssign )
	{
	    pgm.nextToken(); // consume '='
	    val = parse_constant_integer_expression(pgm);
	}

	// register as a global constant variable
	Variable *evar = pgm.addVariable(NULL, ddINT, name, 1, NULL, true);
	evar->set((int)val);
	evar->makeconstant();
	DBG(std::cout << "TokenENUM::parse() " << name << " = " << val << std::endl);
	val++;
    }

    if ( !tn )
	pgm.Throw << "Unterminated enum" << flush;
    pgm.nextToken(); // consume '}'

    // consume optional semicolon
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	pgm.nextToken();

    return NULL;
}

TokenBase *TokenSTATIC::parse(Program &pgm)
{
    DBG(std::cout << "TokenSTATIC::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'static'" << flush;
    // static can appear before other modifiers: static const, static struct, etc.
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() != TokenType::ttDataType )
    {
	// might be a typedef'd identifier
	if ( tn->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)tn)->str;
	    datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	    if ( tdmi != pgm.datatype_map.end() )
	    {
		pgm.nextToken();
		return pgm.parseDeclaration(tdmi->second, true);
	    }
	}
	pgm.Throw(tn) << "Expecting type after 'static'" << flush;
    }
    tn = pgm.nextToken();
    return pgm.parseDeclaration(static_cast<TokenDataType *>(tn), true);
}

// const — consume and pass through to the type that follows
TokenBase *TokenCONST::parse(Program &pgm)
{
    DBG(std::cout << "TokenCONST::parse() — consuming const" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'const'" << flush;
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() == TokenType::ttDataType )
    {
	tn = pgm.nextToken();
	return pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
    }
    // might be typedef'd name
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(tdmi->second);
	}
    }
    pgm.Throw(tn) << "Expecting type after 'const'" << flush;
    return NULL;
}

// extern — consume and pass through to the declaration that follows
TokenBase *TokenEXTERN::parse(Program &pgm)
{
    DBG(std::cout << "TokenEXTERN::parse() — consuming extern" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'extern'" << flush;
    bool prev_extern = pgm.parsing_extern_decl;
    pgm.parsing_extern_decl = true;
    TokenBase *result = NULL;
    bool handled = false;
    try
    {
	if ( tn->type() == TokenType::ttKeyword )
	{
	    handled = true;
	    result = pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
	}
	else if ( tn->type() == TokenType::ttDataType )
	{
	    handled = true;
	    tn = pgm.nextToken();
	    result = pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
	}
	else if ( tn->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)tn)->str;
	    datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	    if ( tdmi != pgm.datatype_map.end() )
	    {
		handled = true;
		pgm.nextToken();
		result = pgm.parseDeclaration(tdmi->second);
	    }
	}
	if ( !handled )
	    pgm.Throw(tn) << "Expecting type after 'extern'" << flush;
    }
    catch ( ... )
    {
	pgm.parsing_extern_decl = prev_extern;
	throw;
    }
    pgm.parsing_extern_decl = prev_extern;
    return result;
}

TokenBase *TokenRESTRICT::parse(Program &pgm)
{
    DBG(std::cout << "TokenRESTRICT::parse() — consuming restrict" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'restrict'" << flush;
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() == TokenType::ttDataType )
    {
	tn = pgm.nextToken();
	return pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
    }
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(tdmi->second);
	}
    }
    pgm.Throw(tn) << "Expecting type after 'restrict'" << flush;
    return NULL;
}

TokenBase *TokenDEFER::parse(Program &pgm)
{
    DBG(std::cout << "TokenDEFER::parse()" << std::endl);

    TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
    if ( !code )
	pgm.Throw(this) << "'defer' must be inside a function or block" << flush;

    // parse the deferred statement
    TokenBase *tn = pgm.nextToken();
    if ( !tn )
	pgm.Throw(this) << "Unexpected end of input after 'defer'" << flush;

    TokenBase *stmt = pgm.parseStatement(tn);
    if ( !stmt )
	pgm.Throw(tn) << "Failed to parse deferred statement" << flush;

    // store on the enclosing compound — compiled in reverse order at scope exit
    code->deferred.push_back(stmt);

    DBG(std::cout << "TokenDEFER::parse() deferred statement added (total: " << code->deferred.size() << ")" << std::endl);

    // return NULL — defer doesn't produce code at this point
    return NULL;
}

// parse switch(expr) { case val: ...; break; default: ...; }
TokenBase *TokenSWITCH::parse(Program &pgm)
{
    DBG(std::cout << "TokenSWITCH::parse()" << std::endl);

    // expect (
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
	pgm.Throw(tn) << "Expecting ( after switch" << flush;

    // parse expression
    expression = pgm.parseExpression(pgm.nextToken(), true);
    if ( !expression )
	pgm.Throw(tn) << "Failed to parse switch expression" << flush;

    // expect )
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkClBrk )
	pgm.Throw(tn) << "Expecting ) after switch expression" << flush;

    // expect {
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrc )
	pgm.Throw(tn) << "Expecting { after switch()" << flush;

    // parse case/default blocks until }
    while ( (tn = pgm.nextToken()) )
    {
	if ( tn->id() == TokenID::tkClBrc )
	    break;
	if ( tn->id() == TokenID::tkCASE )
	{
	    TokenCASE *tc = new TokenCASE();
	    tc->file = tn->file;
	    tc->line = tn->line;
	    tc->column = tn->column;
	    // Parse case value as a constant integer expression. This accepts
	    // plain literals (`case 42:`, `case 'a':`), enum constants, and
	    // parenthesized / negated forms such as `case EOF:` where EOF
	    // expands to `(-1)`. The evaluated int64 is wrapped in a TokenInt
	    // for downstream compile().
	    TokenBase *val_anchor = pgm.peekToken();
	    int64_t case_val = parse_constant_integer_expression(pgm);
	    TokenInt *val_tok = new TokenInt(case_val);
	    if ( val_anchor )
	    {
		val_tok->file = val_anchor->file;
		val_tok->line = val_anchor->line;
		val_tok->column = val_anchor->column;
	    }
	    tc->value = val_tok;
	    // expect : after case value
	    tn = pgm.nextToken();
	    if ( tn->id() != TokenID::tkTerC )
		pgm.Throw(tn) << "Expecting : after case value" << flush;
	    // parse statements until next case/default/}
	    while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkCASE
		    && pgm.peekToken()->id() != TokenID::tkDEFAULT
		    && pgm.peekToken()->id() != TokenID::tkClBrc )
	    {
		TokenBase *stmt = pgm.parseStatement(pgm.nextToken());
		if ( stmt )
		    tc->statements.push_back(stmt);
	    }
	    cases.push_back(tc);
	}
	else if ( tn->id() == TokenID::tkDEFAULT )
	{
	    // expect : after default
	    tn = pgm.nextToken();
	    if ( tn->id() != TokenID::tkTerC )
		pgm.Throw(tn) << "Expecting : after default" << flush;
	    defaultcase = new TokenCASE();
	    defaultcase->value = NULL;
	    defaultcase->file = tn->file;
	    defaultcase->line = tn->line;
	    defaultcase->column = tn->column;
	    // parse statements until next case/}
	    while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkCASE
		    && pgm.peekToken()->id() != TokenID::tkDEFAULT
		    && pgm.peekToken()->id() != TokenID::tkClBrc )
	    {
		TokenBase *stmt = pgm.parseStatement(pgm.nextToken());
		if ( stmt )
		    defaultcase->statements.push_back(stmt);
	    }
	}
	else if ( tn->type() == TokenType::ttDataType
	       || tn->type() == TokenType::ttIdentifier )
	{
	    // C allows variable declarations in a switch body before any
	    // case label — they're unreachable (no case path enters
	    // there) but valid as compile-time declarations. SMAUG's
	    // `switch(SPELL_POWER(skill)) { OBJ_DATA *clone; default: ... }`
	    // is a common form. Parse and discard.
	    DBG(std::cout << "TokenSWITCH::parse() skipping pre-case declaration" << std::endl);
	    pgm.parseStatement(tn);
	}
	else if ( tn->id() == TokenID::tkSemi )
	{
	    // Stray `;` between pre-case declarations and the first case
	    // label — also an empty statement at switch body scope.
	    continue;
	}
	else
	    pgm.Throw(tn) << "Expecting case or default in switch body" << flush;
    }

    DBG(std::cout << "TokenSWITCH::parse() " << cases.size() << " cases" << (defaultcase ? " + default" : "") << std::endl);
    return this;
}

// TokenCASE::parse() is not called directly — TokenSWITCH::parse() handles case parsing
TokenBase *TokenCASE::parse(Program &pgm)
{
    pgm.Throw(this) << "case outside of switch" << flush;
    return NULL;
}

// parse vector<type> — creates DataDefVECTOR and delegates to parseDeclaration
TokenBase *TokenVECTOR::parse(Program &pgm)
{
    DBG(std::cout << "TokenVECTOR::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after vector" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting type inside vector<>" << flush;

    DataDef *elem = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after vector<type" << flush;

    // build composite name and look up or create
    std::string tname = "vector<" + elem->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	// use sizeof of the underlying vector type — all std::vector are same size
	DataDefVECTOR *dd = new DataDefVECTOR(elem, tname, sizeof(std::vector<int64_t>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	// register methods on this parameterization
	Variable *mv;
	if ( elem->is_string() )
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR), DataType::dtSTRING}, (fVOIDFUNC)vector_str_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("pop_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_pop_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("at", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, rtPtr(DataType::dtVECTOR), DataType::dtINT64}, (fVOIDFUNC)vector_str_at, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_clear, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("empty", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_empty, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR), DataType::dtINT64}, (fVOIDFUNC)vector_int_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("pop_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_pop_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("at", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR), DataType::dtINT64}, (fVOIDFUNC)vector_int_at, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_clear, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("empty", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_empty, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
}

// parse map<key_type, val_type>
TokenBase *TokenMAP::parse(Program &pgm)
{
    DBG(std::cout << "TokenMAP::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after map" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting key type inside map<>" << flush;
    DataDef *key = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkComma )
	pgm.Throw(tn) << "Expecting , between key and value types in map<k, v>" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting value type inside map<k, v>" << flush;
    DataDef *val = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after map<k, v" << flush;

    std::string tname = "map<" + key->name + "," + val->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	DataDefMAP *dd = new DataDefMAP(key, val, tname, sizeof(std::map<std::string, int64_t>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	// register methods
	Variable *mv;
	if ( val->is_string() )
	{
	    mv = pgm.addFunction("put", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP), DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)map_str_str_set, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("get", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_str_get, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_str_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP)}, (fVOIDFUNC)map_str_str_size, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("put", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP), DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)map_str_int_set, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("get", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_int_get, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_int_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("erase", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_int_erase, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP)}, (fVOIDFUNC)map_str_int_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP)}, (fVOIDFUNC)map_str_int_clear, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
}

// parse set<type>
TokenBase *TokenSET::parse(Program &pgm)
{
    DBG(std::cout << "TokenSET::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after set" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting type inside set<>" << flush;
    DataDef *elem = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after set<type" << flush;

    std::string tname = "set<" + elem->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	DataDefSET *dd = new DataDefSET(elem, tname, sizeof(std::set<std::string>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	Variable *mv;
	if ( elem->is_string() )
	{
	    mv = pgm.addFunction("insert", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET), DataType::dtSTRING}, (fVOIDFUNC)set_str_insert, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET), DataType::dtSTRING}, (fVOIDFUNC)set_str_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("erase", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET), DataType::dtSTRING}, (fVOIDFUNC)set_str_erase, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET)}, (fVOIDFUNC)set_str_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET)}, (fVOIDFUNC)set_str_clear, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("insert", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET), DataType::dtINT64}, (fVOIDFUNC)set_int_insert, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET), DataType::dtINT64}, (fVOIDFUNC)set_int_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET)}, (fVOIDFUNC)set_int_size, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
}

// parse list<type>
TokenBase *TokenLIST::parse(Program &pgm)
{
    DBG(std::cout << "TokenLIST::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after list" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting type inside list<>" << flush;
    DataDef *elem = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after list<type" << flush;

    std::string tname = "list<" + elem->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	DataDefLIST *dd = new DataDefLIST(elem, tname, sizeof(std::list<int64_t>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	Variable *mv;
	if ( elem->is_string() )
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtSTRING}, (fVOIDFUNC)list_str_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("push_front", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtSTRING}, (fVOIDFUNC)list_str_push_front, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtLIST)}, (fVOIDFUNC)list_str_size, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtINT64}, (fVOIDFUNC)list_int_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("push_front", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtINT64}, (fVOIDFUNC)list_int_push_front, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtLIST)}, (fVOIDFUNC)list_int_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST)}, (fVOIDFUNC)list_int_clear, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
}

TokenBase *Program::parseKeyword(TokenKeyword *tk)
{
    TokenBase *tb = (TokenBase *)tk->parse(*this);
    return tb;
}

// real parsing happens here, code should not be null
TokenBase *Program::parseCompound()
{
    if ( compounds.empty() ) { throw "Internal error -- compound stack empty"; }
    TokenCpnd *code = compounds.top();
    TokenBase *tb = NULL;

    DBG(std::cout << "parseCompound() start" << std::endl);

    while ( (tb=nextToken()) )
    {
	if ( tb->id() == TokenID::tkClBrc )
	{
	    popCompound();
	    DBG(std::cout << "parseCompound() ends" << std::endl);
	    return code;
	}

	if ( (tb=parseStatement(tb)) )
	{
	    DBG(std::cout << "parseStatement() returns token" << std::endl);
	    code->statements.push_back((TokenStmt *)tb);
	}
    }

    DBG(std::cout << "parseCompound() end of input" << std::endl);

    return code;
}

// parse a function definition, can be a forward declaration, or function definition
void Program::parseFunction(DataDef &dd, std::string &id, DataDefCLASS *owner_class,
			    std::vector<DataDef *> *multi_ret)
{
    variable_map_iter vmi;
    funcdef_map_iter fmi;
    datadef_vec_iter dvi;
    FuncDef *func;
    TokenBase *nt = NULL; // next token;
    Variable *var;

    vector<std::string> ids;  // vector of variable names
    TokenDataType *pb;        // parameter basetype
    std::string pid;          // parameter id
    RefType rtype = RefType::rtNone;
    int anon_param_index = 0;

    DBG(cout << "parseFunction(" << dd.name << ' ' << id << ") START" << endl);

    // may have already been declared (e.g. forward decl → definition)
    bool func_already_declared = false;
    if ( (fmi=funcdef_map.find(id)) != funcdef_map.end() )
    {
	func = fmi->second;
	func_already_declared = true;
    }
    else
    {
	func = new FuncDef(dd);
	funcdef_map[id] = func;
	DBG(std::cout << "parseFunction() Added new function declaration type: " << dd.name << " size: " << dd.size << " name: " << id << std::endl);
    }

    // for multi-return functions, inject hidden __retbuf parameter as first arg
    if ( multi_ret && multi_ret->size() > 1 )
    {
	func->return_types = *multi_ret;
	if ( !func_already_declared )
	    func->parameters.push_back(&ddINT64); // void* as int64
	ids.push_back("__retbuf");
	DBG(cout << "parseFunction() injected hidden __retbuf for multi-return (" << multi_ret->size() << " types)" << endl);
    }

    // for class methods, inject hidden __this parameter as first arg
    if ( owner_class )
    {
	if ( !func_already_declared )
	    func->parameters.push_back(&ddINT64); // void* as int64
	ids.push_back("__this");
	DBG(cout << "parseFunction() injected hidden __this parameter for class method" << endl);
    }

    // look for parameters
    while ( (nt=nextToken()) && nt->id() != TokenID::tkClBrk )
    {
	// tolerate C qualifiers/storage hints in parameter lists such as
	// `const char *s` and `register char *s`
	while ( nt && (nt->id() == TokenID::tkCONST || nt->id() == TokenID::tkREGISTER) )
	{
	    nt = nextToken();
	}

	// C `void` as sole parameter means no parameters (e.g. `int f(void)`).
	if ( nt->type() == TokenType::ttDataType
	  && &((TokenDataType *)nt)->definition == &ddVOID
	  && peekToken() && peekToken()->id() == TokenID::tkClBrk )
	{
	    nextToken(); // consume ')'
	    break;
	}

	// detect ... (variadic parameter)
	if ( nt->id() == TokenID::tkDot )
	{
	    TokenBase *d2 = nextToken();
	    TokenBase *d3 = nextToken();
	    if ( !d2 || d2->id() != TokenID::tkDot || !d3 || d3->id() != TokenID::tkDot )
		Throw(nt) << "Expecting '...' for variadic parameter" << flush;
	    func->is_varargs = true;
	    // A prior prototype already owns the hidden varargs slot.
	    if ( !func_already_declared )
		func->parameters.push_back(&ddINT64);
	    ids.push_back("__va_args");
	    DBG(cout << "parseFunction() detected varargs, injected __va_args" << endl);
	    // next token should be )
	    nt = nextToken();
	    if ( nt->id() != TokenID::tkClBrk )
		Throw(nt) << "Expecting ')' after '...'" << flush;
	    break;
	}

	// handle 'struct Tag' as parameter type
	if ( nt->id() == TokenID::tkSTRUCT )
	{
	    TokenBase *tag_nt = nextToken();
	    if ( tag_nt->type() != TokenType::ttIdentifier )
		Throw(tag_nt) << "Expecting struct name after 'struct' in parameters" << flush;
	    std::string sname = ((TokenIdent *)tag_nt)->str;
	    datadef_map_iter sdmi = struct_map.find(sname);
	    if ( sdmi == struct_map.end() )
	    {
		// C permits pointers/references to incomplete struct types in
		// parameter lists, so synthesize a forward declaration on demand.
		DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
		struct_map[sname] = fwd;
		sdmi = struct_map.find(sname);
	    }
	    std::string tname("struct ");
	    tname.append(sname);
	    pb = new TokenDataType(tname.c_str(), *sdmi->second);
	}
	else
	if ( nt->type() != TokenType::ttDataType )
	{
	    // also check datatype_map for typedef'd names (e.g. CHAR_DATA)
	    if ( nt->type() == TokenType::ttIdentifier )
	    {
		std::string tname = ((TokenIdent *)nt)->str;
		datatype_map_iter tdmi = datatype_map.find(tname);
		if ( tdmi != datatype_map.end() )
		{
		    pb = tdmi->second;
		}
		else
		{
		    DBG(std::cerr << "parseFunction() params: failed to obtain basetype" << std::endl);
		    Throw(nt) << "Failed to find type when parsing function parameters" << flush;
		}
	    }
	    else
	    {
		DBG(std::cerr << "parseFunction() params: failed to obtain basetype" << std::endl);
		Throw(nt) << "Failed to find type when parsing function parameters" << flush;
	    }
	}
	else
	    pb = (TokenDataType *)nt;
	rtype = RefType::rtValue;
	DataDef *param_dd = &pb->definition;
grabnt:
	// grab the next token
	if ( !peekToken() )
	    Throw(nt) << "Unexpected end of file parsing function parameters" << flush;

	nt = nextToken();

	if ( nt->id() == TokenID::tkBand )
	{
	    rtype = RefType::rtReference;
	    DBG(std::cout << "parseFunction() setting reference token " << std::endl);
//	    pb->definition.setRef(RefType::rtReference);
	    goto grabnt;
	}
	if ( nt->id() == TokenID::tkStar )
	{
	    rtype = RefType::rtPointer;
	    param_dd = getPointerType(param_dd);
	    DBG(std::cout << "parseFunction() pointer param: " << param_dd->name << std::endl);
	    goto grabnt;
	}
	if ( is_restrict_token(nt) || nt->id() == TokenID::tkCONST )
	    goto grabnt;
	if ( nt->id() == TokenID::tkComma || nt->id() == TokenID::tkClBrk )
	{
	    pid = "__anon_param_" + std::to_string(anon_param_index++);
	    goto paramdecl;
	}
	if ( nt->id() == TokenID::tkOpBrk )
	{
	    TokenBase *inner = nextToken();
	    if ( inner && inner->id() == TokenID::tkStar )
	    {
		// Function-pointer parameter declarator, e.g.
		// `void (*markfn)(void *)`. Treat the parameter type as an
		// opaque function pointer for now.
		param_dd = &ddINT64;
		rtype = RefType::rtValue;

		nt = nextToken();
		while ( nt && (is_restrict_token(nt) || nt->id() == TokenID::tkCONST) )
		    nt = nextToken();
		if ( !is_contextual_identifier_token(nt) )
		    Throw(nt) << "Expecting identifier in function pointer parameter" << flush;
		pid = contextual_identifier_name(nt);

		nt = nextToken();
		if ( !nt || nt->id() != TokenID::tkClBrk )
		    Throw(nt ? nt : inner) << "Expected ')' after function pointer parameter name" << flush;
		nt = nextToken();
		if ( !nt || nt->id() != TokenID::tkOpBrk )
		    Throw(nt ? nt : inner) << "Expected '(' after function pointer parameter name" << flush;

		int fp_depth = 1;
		while ( fp_depth > 0 )
		{
		    nt = nextToken();
		    if ( !nt )
			Throw(inner) << "Unexpected end of input in function pointer parameter" << flush;
		    if ( nt->id() == TokenID::tkOpBrk )
			++fp_depth;
		    else if ( nt->id() == TokenID::tkClBrk )
			--fp_depth;
		}

		nt = nextToken();
		goto paramdecl;
	    }
	    Throw(inner ? inner : nt) << "Unsupported parenthesized parameter declarator" << flush;
	}
	if ( !is_contextual_identifier_token(nt) )
	{
	    Throw(nt) << "Expecting identifier after type" << flush;
	}

	// grab identifier string
	pid = contextual_identifier_name(nt);
	if ( !peekToken() )
	    Throw(nt) << "Expecting token after identifier" << flush;

	nt = nextToken();

	// Array parameters decay to pointers in C, so accept declarators like
	// `char * const argv[]` by consuming each [] suffix and promoting the
	// parameter type by one pointer level.
	while ( nt && nt->id() == TokenID::tkOpSqr )
	{
	    if ( peekToken() && peekToken()->id() != TokenID::tkClSqr )
		(void)parse_constant_integer_expression(*this);
	    nt = nextToken();
	    if ( !nt || nt->id() != TokenID::tkClSqr )
		Throw(nt ? nt : peekToken()) << "Expected ']' in parameter array declarator" << flush;
	    param_dd = getPointerType(param_dd);
	    rtype = RefType::rtPointer;
	    nt = nextToken();
	}

paramdecl:
	// parameter declaration
	if ( nt->id() == TokenID::tkComma || nt->id() == TokenID::tkClBrk )
	{
	    // If this is a definition following a forward declaration, the
	    // function already has its parameter DataDefs — don't re-push.
	    // Just record the name so the body's `ids[]` / variable binding
	    // stays aligned.
	    if ( func_already_declared )
	    {
		ids.push_back(pid);
	    }
	    else if ( !func->findParameter(pid) )
	    {
		ids.push_back(pid);
		if ( rtype == RefType::rtReference && pb->definition.rawtype() == DataType::dtSTRING )
		    func->parameters.push_back(&ddSTRINGref);
		else if ( rtype == RefType::rtPointer )
		    func->parameters.push_back(param_dd);
		else
		    func->parameters.push_back(&pb->definition);
		DBG(std::cout << "Added new parameter declaration type: " << dd.name << " size: "
		    << dd.size << " name: " << pid << " ptr: " << &dd << std::endl);
	    }
	    else
	    {
		DBG(std::cerr << "parseFunction() params: duplicate parameter name " << pid << std::endl);
		Throw(nt) << "Duplicate parameter name" << flush;
	    }
	    if ( nt->id() == TokenID::tkClBrk )
		break;
	}
    }

    if ( !nt )
    {
	DBG(std::cerr << "parseFunction() expecting more tokens, missing closing bracket" << std::endl);
	Throw << "Missing closing bracket" << flush;
    }

    nt = nextToken();

    Method *method;

    if ( (var=tkProgram->findVariable(id)) )
    {
	var->type = func;
	method = (Method *)var->data;
    }
    else
    {
	var = addVariable(NULL, *func, id);
	method = NULL;
    }

    // semicolon means this is just a function declaration
    if ( nt->id() == TokenID::tkSemi )
    {
	if ( !method )
	{
	    method = new Method(*var);
	    var->data = (void *)method;
	}
	if ( owner_class )
	    method->owner_class = owner_class;
	DBG(std::cout << "parseFunction() forward declaration of function " << id << std::endl);
	return;
    }

    // Definitions must own a fresh Method instance. Some prior declaration
    // paths (notably SMAUG macro expansions) leave a non-null var->data that
    // is not a valid Method object, so reusing it corrupts method->parameters.
    method = new Method(*var);
    var->data = (void *)method;

    // need to see a brace to define a function
    if ( nt->id() != TokenID::tkOpBrc )
    {
	// throw error
	Throw(nt) << "Expecting brace after function declaration" << flush;
    }

    if ( owner_class )
	method->owner_class = owner_class;

    DataDef *d;
    Variable *v;
    int i = 0;

    DBG(cout << "parseFunction() param loop: func->parameters.size()=" << func->parameters.size()
	<< " ids.size()=" << ids.size() << " method=" << (void*)method << endl);
    for ( dvi = func->parameters.begin(); dvi != func->parameters.end(); ++dvi )
    {
	d = *dvi;
	DBG(cout << "parseFunction() adding parameter variable " << ids[i] << endl);
	v = new Variable(ids[i++], *d, 1, NULL, false);
	v->flags |= vfPARAM;
	method->parameters.push_back(v);
	DBG(cout << "parseFunction() pushed param, method->parameters.size()=" << method->parameters.size() << endl);
    }

    pushCompound();
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    if ( code )
    {
	DBG(cout << "parseFunction() setting code->method" << endl);
	code->method = method;
    }
    else
    {
	DBG(cout << "parseFunction() code = NULL" << endl);
    }

    TokenFunc *tf = new TokenFunc(*var);
    DBG(cout << "parseFunction() calling parseCompound()" << endl);
    TokenCpnd *tc = dynamic_cast<TokenCpnd *>(parseCompound());

    tf->method = method;
    tf->parent = tc->parent;
    tf->variables = tc->variables;
    tf->statements = tc->statements;
    tf->deferred = tc->deferred;

    DBG(cout << "parseFunction() calling ast.push" << endl);
    ast.push(tf);
    pending_funcs.push_back(tf);

    DBG(cout << "parseFunction(" << id << ") END" << endl);
}

// parse a lambda expression: [](type arg, ...) { body }
// Returns a TokenVar referencing the lambda's anonymous function variable.
// The lambda is pushed onto ast as a top-level TokenFunc so it compiles
// before the enclosing function (asmjit can't nest addFunc/endFunc).
TokenBase *Program::parseLambda()
{
    static int lambda_counter = 0;

    DBG(cout << "parseLambda() START" << endl);

    // we already consumed '[', peek at next token
    // [](params) { body }       — pure lambda (no capture)
    // [int](params) { body }    — pure lambda with return type
    // [&](params) { body }      — capture all outer vars by reference
    TokenBase *tn = nextToken();
    DataDef *rettype = &ddVOID;
    bool is_capturing = false;

    // check for [&] capture syntax
    if ( tn->id() == TokenID::tkBand )
    {
	is_capturing = true;
	tn = nextToken(); // consume &, expect ]
    }
    else if ( tn->type() == TokenType::ttDataType )
    {
	rettype = &((TokenDataType *)tn)->definition;
	DBG(cout << "parseLambda() return type: " << rettype->name << endl);
	tn = nextToken();
    }

    if ( tn->id() != TokenID::tkClSqr )
	Throw(tn) << "Expecting ] in lambda expression" << flush;

    // expect '('
    tn = nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
	Throw(tn) << "Expecting ( after lambda [...]" << flush;

    // generate unique name
    std::string lambda_name = "__lambda_" + std::to_string(lambda_counter++);

    DBG(cout << "parseLambda() name: " << lambda_name << endl);

    // create FuncDef
    FuncDef *func = new FuncDef(*rettype);
    funcdef_map[lambda_name] = func;
    func->has_captures = is_capturing;

    if ( is_capturing )
    {
	// Collect all currently visible vars from the enclosing compound chain
	// These are "potential captures" — whichever ones the body actually uses
	TokenCpnd *outer = compounds.empty() ? NULL : compounds.top();
	while ( outer )
	{
	    for ( auto *v : outer->variables )
		func->potential_captures.push_back(v);
	    // also capture method parameters from the outer scope
	    if ( outer->method )
		for ( auto *p : outer->method->parameters )
		    func->potential_captures.push_back(p);
	    outer = outer->parent;
	}
	// Pre-register env as first parameter in FuncDef (user params appended after)
	func->parameters.push_back(&ddINT64);
	DBG(cout << "parseLambda() [&] capturing " << func->potential_captures.size() << " outer vars" << endl);
    }

    // parse parameters (same pattern as parseFunction)
    std::vector<std::string> param_ids;
    TokenDataType *pb;

    while ( (tn=nextToken()) && tn->id() != TokenID::tkClBrk )
    {
	if ( tn->type() != TokenType::ttDataType )
	    Throw(tn) << "Expecting type in lambda parameter list" << flush;

	pb = (TokenDataType *)tn;
	tn = nextToken();

	if ( !is_contextual_identifier_token(tn) )
	    Throw(tn) << "Expecting identifier in lambda parameter list" << flush;

	std::string pid = contextual_identifier_name(tn);
	param_ids.push_back(pid);
	func->parameters.push_back(&pb->definition);

	DBG(cout << "parseLambda() param: " << pb->definition.name << ' ' << pid << endl);

	// peek for comma or closing paren
	tn = peekToken();
	if ( tn && tn->id() == TokenID::tkComma )
	    nextToken(); // consume comma
    }

    // create Variable and Method (same as parseFunction)
    Variable *var = addVariable(NULL, *func, lambda_name);
    Method *method = new Method(*var);
    var->data = (void *)method;

    // if capturing: create hidden env_param at position 0 in method->parameters
    if ( is_capturing )
    {
	std::string env_name = "__env";
	Variable *env_pv = new Variable(env_name, ddINT64, 1, NULL, false);
	env_pv->flags |= vfPARAM;
	method->env_param = env_pv;
	method->parameters.push_back(env_pv); // will be moved to front below
    }

    // add user parameters to method
    for ( size_t i = 0; i < param_ids.size(); ++i )
    {
	// user params start at index 1 in func->parameters when capturing (0 is env)
	size_t fi = is_capturing ? i + 1 : i;
	Variable *pv = new Variable(param_ids[i], *func->parameters[fi], 1, NULL, false);
	pv->flags |= vfPARAM;
	method->parameters.push_back(pv);
    }

    // expect '{' for the body
    tn = nextToken();
    if ( tn->id() != TokenID::tkOpBrc )
	Throw(tn) << "Expecting { for lambda body" << flush;

    // push compound scope and parse the body
    pushCompound();
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    if ( code )
	code->method = method;

    TokenFunc *tf = new TokenFunc(*var);
    DBG(cout << "parseLambda() calling parseCompound()" << endl);
    TokenCpnd *tc = dynamic_cast<TokenCpnd *>(parseCompound());

    tf->method = method;
    tf->parent = tc->parent;
    tf->variables = tc->variables;
    tf->statements = tc->statements;
    tf->deferred = tc->deferred;

    // push the lambda as a top-level function in the AST
    // It will be compiled before the enclosing function since
    // the enclosing function's ast.push happens after parseCompound returns.
    DBG(cout << "parseLambda() pushing " << lambda_name << " onto ast" << endl);
    ast.push(tf);
    pending_funcs.push_back(tf);

    DBG(cout << "parseLambda() END — returning TokenVar for " << lambda_name << endl);

    // return a TokenVar that references the lambda function variable
    // When compiled, TokenVar::compile() emits the function's address
    return new TokenVar(*var);
}

// parse either a variable declaration, or a function declaration
TokenBase *Program::parseDeclaration(TokenDataType *tb, bool is_static)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenBase *nt; // next token;
    Variable *var;
    string id;
    bool gotstatic = is_static;

    DBG(std::cout << "parseDeclaration(" << tb->str << ") START" << std::endl);

    // check for pointer declarator(s): type * [*...] identifier.
    // base_type is the declared type without any `*`s — comma-continuations
    // later in this function start fresh from base_type because each var
    // in `char *p, *q;` has its own `*`s, not cumulative.
    DataDef *base_type = &tb->definition;
    DataDef *decl_type = base_type;
    // Function-pointer typedefs (DataDefFPTR) already represent pointers;
    // `DO_FUN *cmd;` and `DO_FUN cmd;` both name a function-pointer variable.
    bool is_fnptr_base = (dynamic_cast<DataDefFPTR *>(base_type) != NULL);
    // C allows CV-qualifiers (`const`/`restrict`) interleaved with
    // pointer stars: `type const *p`, `int * const *xpp`, etc.
    // Loop consuming any qualifier-or-star sequence; qualifiers are
    // treated as no-ops, stars stack the pointer level.
    while ( peekToken()
	 && (peekToken()->id() == TokenID::tkMul
	  || is_post_pointer_qualifier_token(peekToken())) )
    {
	if ( peekToken()->id() == TokenID::tkMul )
	{
	    nextToken(); // consume '*'
	    if ( !is_fnptr_base )
		decl_type = getPointerType(decl_type);
	    DBG(std::cout << "parseDeclaration() pointer: " << decl_type->name << std::endl);
	}
	else
	    nextToken(); // consume const/restrict
    }

    if ( !peekToken() )
	Throw(tb) << "Unexpected end of data: Expecting identifier after type" << flush;

    // Function-pointer variable declaration:
    //   RET (*name)(params);
    if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
    {
	TokenBase *open = nextToken(); // consume '('
	TokenBase *star = nextToken();
	if ( star && star->id() == TokenID::tkMul )
	{
	    TokenBase *name_tok = nextToken();
	    if ( !name_tok || !is_contextual_identifier_token(name_tok) )
		Throw(name_tok ? name_tok : open) << "Expecting identifier in function pointer declaration" << flush;
	    id = contextual_identifier_name(name_tok);
	    TokenBase *rbrk = nextToken();
	    if ( !rbrk || rbrk->id() != TokenID::tkClBrk )
		Throw(rbrk ? rbrk : open) << "Expecting ')' after function pointer name" << flush;
	    TokenBase *param_open = nextToken();
	    if ( !param_open || param_open->id() != TokenID::tkOpBrk )
		Throw(param_open ? param_open : open) << "Expecting '(' for function pointer parameter list" << flush;

	    FuncDef *func = parseFnPtrParams(*decl_type);
	    DataDefFPTR *fptr_type = new DataDefFPTR(func);
	    bool alloc = (!code || gotstatic) ? true : false;
	    var = addVariable(code, *fptr_type, id, 1, NULL, alloc);
	    if ( gotstatic )
		var->flags |= vfSTATIC;
	    TokenDecl *td = new TokenDecl(*var);
	    td->file = tb->file;
	    td->line = tb->line;
	    td->column = tb->column;
	    return td;
	}
	pushToken(star);
	pushToken(open);
    }

    nt = nextToken();

    if ( !is_contextual_identifier_token(nt) )
    {
	DBG(cerr << "parseDeclaration() nt->type()=" << (int)nt->type() << endl);
	Throw(nt) << "Expecting identifier after type" << flush;
    }
    // grab identifier string
    id = contextual_identifier_name(nt);
    DBG(std::cout << "parseDeclaration() identifier: " << id << std::endl);

    if ( !(nt=peekToken()) )
	Throw << "expecting token after identifier" << flush;

    // auto type inference: auto fn = func_name; or auto fn = [](params) { body };
    if ( &tb->definition == &ddAUTO )
    {
	if ( nt->id() != TokenID::tkAssign )
	    Throw(tb) << "'auto' requires an initializer" << flush;

	// consume '='
	nextToken();
	TokenBase *rhs_tok = peekToken();

	Variable *rhs_var = NULL;
	TokenBase *rhs_node = NULL;

	if ( rhs_tok && rhs_tok->id() == TokenID::tkOpSqr )
	{
	    // lambda: auto fn = [](params) { body };
	    nextToken(); // consume '['
	    rhs_node = parseLambda();
	    rhs_var = &(dynamic_cast<TokenVar *>(rhs_node)->var);
	}
	else if ( rhs_tok && rhs_tok->type() == TokenType::ttIdentifier )
	{
	    // named function: auto fn = func_name;
	    nextToken(); // consume identifier
	    rhs_var = findVariable(((TokenIdent *)rhs_tok)->str);
	    if ( !rhs_var || !rhs_var->type->is_function() )
		Throw(tb) << "'auto' type deduction requires a function name or lambda" << flush;
	    rhs_node = new TokenVar(*rhs_var);
	}
	else
	{
	    Throw(tb) << "'auto' type deduction requires a function name or lambda" << flush;
	}

	// consume the semicolon
	TokenBase *semi = peekToken();
	if ( semi && semi->id() == TokenID::tkSemi )
	    nextToken();

	// create a DataDefFPTR wrapping the target function's FuncDef
	FuncDef *target_func = (FuncDef *)rhs_var->type;
	DataDefFPTR *fptr_type = new DataDefFPTR(target_func);

	bool alloc = (!code || gotstatic) ? true : false;
	var = addVariable(code, *fptr_type, id, 1, NULL, alloc);
	TokenDecl *td = new TokenDecl(*var);
	td->file = tb->file;
	td->line = tb->line;
	td->column = tb->column;

	// build the assignment AST
	TokenAssign *assign = new TokenAssign();
	assign->file = tb->file;
	assign->line = tb->line;
	assign->column = tb->column;
	assign->left = new TokenVar(*var);
	assign->right = rhs_node;
	td->initialize = assign;

	DBG(std::cout << "parseDeclaration() auto: " << id << " = " << rhs_var->name << std::endl);
	return td;
    }

    // Check for C fixed-size array declaration: type id[N][M]... or type id[] = {...}
    std::vector<uint32_t> arr_dims;
    while ( nt && nt->id() == TokenID::tkOpSqr )
    {
	nextToken(); // consume [
	TokenBase *peek = peekToken();
	if ( peek && peek->id() == TokenID::tkClSqr )
	{
	    // [] — size to be inferred from initializer
	    nextToken(); // consume ]
	    arr_dims.push_back(0);
	}
	else
	{
	    int64_t n = parse_constant_integer_expression(*this);
	    if ( n <= 0 )
		Throw(tb) << "Fixed-size array dimension must be positive" << flush;
	    arr_dims.push_back((uint32_t)n);
	    TokenBase *cl = nextToken();
	    if ( !cl || cl->id() != TokenID::tkClSqr )
		Throw(cl ? cl : tb) << "Expected ] in array declaration" << flush;
	}
	nt = peekToken();
	if ( !nt )
	    Throw(tb) << "Unexpected end of data in array declaration" << flush;
    }

    // Preserve pointer semantics for `char *p = "literal";`.
    // Only real array declarators (`char buf[] = "literal";`) should take the
    // char-array string-initializer path below.

    // variable declaration
    if ( nt->id() == TokenID::tkSemi || nt->id() == TokenID::tkAssign
      || nt->id() == TokenID::tkComma )
    {
	// parse brace-enclosed initializer list for fixed-size arrays and structs
	std::vector<TokenBase *> init_list;
	// Only real user-defined structs/classes accept brace init.
	// Built-in class types (std::string, ostream, etc.) use DataDefCLASS
	// but have a concrete DataType; user-defined structs/classes use
	// dtRESERVED. Discriminate on that.
	bool is_struct_init = arr_dims.empty()
	    && dynamic_cast<DataDefSTRUCT *>(decl_type) != NULL
	    && decl_type->type() == DataType::dtRESERVED;
	if ( nt->id() == TokenID::tkAssign && (!arr_dims.empty() || is_struct_init) )
	{
	    // peek past '=' to see if we have { (brace list) or "..." (string lit for char arr)
	    nextToken(); // consume '='
	    TokenBase *peek0 = peekToken();
	    if ( !peek0 )
		Throw(nt) << "Expected initializer after '='" << flush;

	    // String-literal char-array init: char buf[] = "hello";
	    if ( !arr_dims.empty()
	      && peek0->type() == TokenType::ttString
	      && decl_type == &ddCHAR && arr_dims.size() == 1 )
	    {
		// C concatenates adjacent string literals, so consume all
		// immediately consecutive ttString tokens here.
		while ( peekToken() && peekToken()->type() == TokenType::ttString )
		{
		    TokenBase *strtok = nextToken();
		    const std::string &s = ((TokenStr *)strtok)->str;
		    for ( char c : s )
			init_list.push_back(new TokenInt((int64_t)(unsigned char)c));
		}
		// C89/C99: if the explicit array size exactly matches the
		// string length, the null terminator is omitted (e.g.
		// `char c[3] = "abc";` is valid). For inferred sizes and
		// any size larger than the literal, append '\0'.
		if ( arr_dims[0] == 0 || arr_dims[0] > init_list.size() )
		    init_list.push_back(new TokenInt(0)); // null terminator
	    }
	    else if ( is_struct_init && peek0->id() != TokenID::tkOpBrc )
	    {
		// struct-copy init: `struct S a = expr;` where `expr` is
		// another struct-typed lvalue. Push '=' back and fall
		// through to the normal `=` init path below; TokenAssign's
		// struct-to-struct branch emits a bytewise memcpy at
		// compile time.
		pushToken(nt);
	    }
	    else
	    {
	    if ( peek0->id() != TokenID::tkOpBrc )
		Throw(nt) << "Expected '{' or string literal for initializer" << flush;
	    nextToken(); // consume '{'
	    // parse comma-separated elements up to '}'. Each element may itself
	    // be a brace-list (for array-of-structs or nested struct members).
	    while ( true )
	    {
		TokenBase *look = peekToken();
		if ( !look )
		    Throw(tb) << "Unexpected end of data in initializer" << flush;
		if ( look->id() == TokenID::tkClBrc )
		{
		    nextToken(); // consume '}'
		    break;
		}
		if ( look->id() == TokenID::tkOpBrc )
		{
		    // Nested brace-list → TokenStructLit (one element of an
		    // array-of-structs, or a nested struct member value). Recursive
		    // so a struct member can itself be initialized with a brace list,
		    // e.g. SMAUG's liq_type[]:
		    //   { "water", "clear", { 0, 1, 10 } },
		    std::function<TokenStructLit *(void)> read_struct_lit;
		    read_struct_lit = [&]() -> TokenStructLit * {
			nextToken(); // consume '{'
			TokenStructLit *slit = new TokenStructLit();
			while ( true )
			{
			    TokenBase *iln = peekToken();
			    if ( !iln )
				Throw(tb) << "Unexpected end of data in nested initializer" << flush;
			    if ( iln->id() == TokenID::tkClBrc )
			    {
				nextToken(); // consume '}'
				break;
			    }
			    if ( iln->id() == TokenID::tkOpBrc )
				slit->inits.push_back(read_struct_lit());
			    else
				slit->inits.push_back(parseExpression(nextToken()));
			    TokenBase *isep = peekToken();
			    if ( isep && isep->id() == TokenID::tkComma )
				nextToken();
			}
			return slit;
		    };
		    init_list.push_back(read_struct_lit());
		}
		else
		{
		    TokenBase *expr = parseExpression(nextToken());
		    init_list.push_back(expr);
		}
		TokenBase *sep = peekToken();
		if ( sep && sep->id() == TokenID::tkComma )
		    nextToken(); // consume ','
	    }
	    }
	    // Infer size for arrays with dims[0] == 0; validate count
	    if ( !arr_dims.empty() )
	    {
		if ( arr_dims[0] == 0 )
		    arr_dims[0] = (uint32_t)init_list.size();
		if ( init_list.size() > (size_t)arr_dims[0] )
		    Throw(tb) << "Too many initializers for array (expected " << arr_dims[0] << ")" << flush;
	    }
	}
	else if ( !arr_dims.empty() )
	{
	    for ( auto d : arr_dims )
		if ( d == 0 )
		{
		    if ( !parsing_extern_decl )
			Throw(nt) << "Array size missing and no initializer" << flush;
		}
	}

	bool alloc = parsing_extern_decl ? false : ((!code || gotstatic) ? true : false);
	uint32_t elem_count = 1;
	for ( auto d : arr_dims ) elem_count *= (d == 0 ? 1 : d);
	var = addVariable(code, *decl_type, id, elem_count, NULL, alloc);
	if ( gotstatic )
	    var->flags |= vfSTATIC;
	if ( parsing_extern_decl )
	    var->flags |= vfEXTERN;
	else
	    var->flags &= ~vfEXTERN;
	if ( !arr_dims.empty() )
	{
	    var->dims = arr_dims;
	    var->flags |= vfFIXEDARRAY;
	    // Global (or static-local) fixed arrays can't live on the JIT
	    // stack — allocate a heap buffer now. voperand detects var->data
	    // and loads the absolute address as the base pointer.
	    if ( alloc && !var->data )
	    {
		size_t total = (size_t)decl_type->size * (size_t)elem_count;
		if ( total == 0 ) total = 1;
		var->data = calloc(1, total);
		var->flags |= vfALLOC;
	    }
	}
	TokenDecl *td = new TokenDecl(*var);

	td->file = tb->file;
	td->line = tb->line;
	td->column = tb->column;
	td->init_list = init_list;

	if ( nt->id() == TokenID::tkAssign && arr_dims.empty() && init_list.empty() )
	{
	    DBG(std::cout << "parseDeclaration() calling td->initialize = parseExpression" << std::endl);
	    // conditional=true so `;` stops without being consumed, which lets
	    // the comma-continuation loop below distinguish "end of decl" (peek
	    // is `;`) from "more decls" (peek is `,` or the next identifier).
	    td->initialize = parseExpression(new TokenVar(*var), true);
	    TokenAssign *ta = dynamic_cast<TokenAssign *>(td->initialize);
	    TokenVar *lhs_var = ta ? dynamic_cast<TokenVar *>(ta->left) : NULL;
	    if ( !ta || !lhs_var || &lhs_var->var != var )
	    {
		TokenAssign *wrap = new TokenAssign();
		wrap->file = tb->file;
		wrap->line = tb->line;
		wrap->column = tb->column;
		wrap->left = new TokenVar(*var);
		wrap->right = td->initialize;
		td->initialize = wrap;
	    }
	}

	// Comma-continuation: `int a, b = 1, c;` or
	// `char a[8], b[16];` — after the first declarator, if the next token is
	// `,` (or parseExpression already consumed one and left an identifier/`*`
	// next), inject a clone of the base type token back into the stream.
	// parseCompound's next iteration sees it as a fresh `type ...`
	// statement and calls parseDeclaration recursively, which handles
	// pointer decorators, array declarators, initializers, and further commas.
	{
	    TokenBase *peek = peekToken();
	    bool have_comma = peek && peek->id() == TokenID::tkComma;
	    if ( have_comma )
	    {
		nextToken(); // consume ','
		peek = peekToken();
	    }
	    // Either we just consumed ',' and expect another decl, or
	    // parseExpression already consumed ',' and peek is the next one.
	    bool looks_like_next_decl = peek
		&& (peek->id() == TokenID::tkMul
		 || peek->type() == TokenType::ttIdentifier);
	    if ( have_comma || (looks_like_next_decl
		&& nt->id() == TokenID::tkAssign) ) // only infer no-comma case when we had an init
	    {
		if ( !looks_like_next_decl )
		    Throw(peek ? peek : tb) << "Expecting identifier after ',' in declaration" << flush;
		// Push back a synthetic base-type token so the next parseStatement
		// sees it as the start of a new declaration.
		pushToken(tb->clone());
	    }
	}

	DBG(std::cout << "parseDeclaration() returning" << std::endl);

	return td;
    }

    nt = nextToken();
    if ( nt->id() != TokenID::tkOpBrk )
    {
	DBG(std::cerr << "parseDeclaration() throwing token " << (int)nt->id() << std::endl);
	Throw(nt) << "unexpected token type " << (int)nt->type() << flush;
    }

    DBG(std::cout << "parseDeclaration() returning" << std::endl);

    parseFunction(*decl_type, id);

    return NULL;
}

// parse a statement into the AST
TokenBase *Program::parseStatement(TokenBase *tb)
{
    DBG(cout << "parseStatement() start" << endl);
    switch(tb->type())
    {
	// for now, just ignore whitespace and comments
	// this shouldn't occur though, as they should already be culled
	case TokenType::ttSpace:
	case TokenType::ttTab:
	case TokenType::ttEOL:
	case TokenType::ttComment:
	    break;

	// if we start with a type (i.e. int), then this could
	// either be a function or a variable declaration
	case TokenType::ttDataType:
	    DBG(std::cout << "parseStatement(" << (int)tb->type() << ") calling parseDeclaration" << std::endl);
	    return parseDeclaration((TokenDataType *)tb);
//	    break;

	case TokenType::ttSymbol:
	    if ( tb->id() == TokenID::tkOpBrc )
	    {
		pushCompound();
		return parseCompound();
	    }
	    if ( tb->id() == TokenID::tkClBrc )
	    {
		popCompound();
		return tb;
	    }
	    if ( tb->id() == TokenID::tkSemi )
		return tb;

	    DBG(std::cerr << "parseStatement() throwing token " << (char)tb->get() << std::endl);
	    Throw(tb) << "unexpected token type " << (int)tb->type() << flush;

	// if we start with an operator or an identifier, then this could be an
	// assignment or a function call
	case TokenType::ttIdentifier:
	    DBG(std::cout << "parseStatement() got identifier " << ((TokenIdent *)tb)->str << std::endl);
	    // Label definition: `name:` at statement position. `:` alone is
	    // tkTerC (it shares the id with the ternary-`:`), while `::` is
	    // the separate tkNS token emitted by the lexer, so a peek of
	    // tkTerC here cannot be a namespace prefix. This has to run
	    // before the datatype-identifier / namespace / `:=` branches
	    // because `name:` at statement position is otherwise ambiguous.
	    if ( peekToken() && peekToken()->id() == TokenID::tkTerC )
	    {
		std::string lname = ((TokenIdent *)tb)->str;
		nextToken(); // consume ':'
		DBG(std::cout << "parseStatement() label definition: " << lname << std::endl);
		return new TokenLabel(lname);
	    }
	    // check if identifier is a user-defined type (class/struct registered in datatype_map)
	    {
		std::string tname = ((TokenIdent *)tb)->str;
		datatype_map_iter dmi = datatype_map.find(tname);
		if ( dmi == datatype_map.end() )
		{
		    // try lazy type registration from #include headers
		    DataDef *dd = lazy_resolve_type(tname);
		    if ( dd )
		    {
			// register as a datatype so future lookups find it
			// TODO: create a TokenDataType wrapper for the resolved type
		    }
		}
		else
		{
		    DBG(std::cout << "parseStatement() identifier is a registered type, calling parseDeclaration" << std::endl);
		    return parseDeclaration(dmi->second);
		}
	    }
	    // namespace resolution: set current namespace and re-enter parseStatement
	    if ( peekToken() && peekToken()->id() == TokenID::tkNS )
	    {
		std::string ns_name = ((TokenIdent *)tb)->str;
		namespace_map_t::iterator nsi = namespace_map.find(ns_name);
		if ( nsi != namespace_map.end() )
		{
		    nextToken(); // consume ::
		    current_namespace = ns_name;
		    TokenBase *result = parseStatement(nextToken());
		    current_namespace.clear();
		    return result;
		}
	    }
	    // := short declaration: identifier := expression;
	    // also handles multi-return: a, b := func();
	    if ( peekToken() && (peekToken()->id() == TokenID::tkColEq
		|| peekToken()->id() == TokenID::tkComma) )
	    {
		std::string first_id = ((TokenIdent *)tb)->str;

		// check if this is a multi-variable declaration: a, b := func()
		if ( peekToken()->id() == TokenID::tkComma )
		{
		    // collect identifiers: a, b, c, ... := expr
		    std::vector<std::string> ids;
		    ids.push_back(first_id);
		    while ( peekToken() && peekToken()->id() == TokenID::tkComma )
		    {
			nextToken(); // consume comma
			TokenBase *next_id = nextToken();
			if ( next_id->type() != TokenType::ttIdentifier )
			    Throw(next_id) << "Expecting identifier in multi-return declaration" << flush;
			ids.push_back(((TokenIdent *)next_id)->str);
		    }
		    // expect :=
		    if ( !peekToken() || peekToken()->id() != TokenID::tkColEq )
		    {
			// not a multi-return declaration — we consumed commas we shouldn't have
			// this shouldn't happen in practice since comma after identifiers
			// only makes sense before :=
			Throw(tb) << "Expecting := after identifier list" << flush;
		    }
		    nextToken(); // consume :=

		    // parse the RHS function call
		    TokenBase *rhs = parseExpression(nextToken());

		    // look up the function's return_types to infer variable types
		    FuncDef *func = NULL;
		    if ( rhs->type() == TokenType::ttCallFunc )
		    {
			TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(rhs);
			if ( tcf->var.type->basetype() == BaseType::btFunct )
			    func = (FuncDef *)tcf->var.type;
		    }

		    // create a TokenCpnd-like wrapper that declares all variables
		    // and generates the multi-return unpack
		    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();

		    // create variables with inferred types from return_types
		    std::vector<Variable *> vars;
		    for ( size_t i = 0; i < ids.size(); ++i )
		    {
			DataDef *vtype = &ddINT64; // default
			if ( func && i < func->return_types.size() )
			    vtype = func->return_types[i];
			else if ( func && i == 0 )
			    vtype = &func->returns;
			bool alloc = (!code) ? true : false;
			Variable *v = addVariable(code, *vtype, ids[i], 1, NULL, alloc);
			vars.push_back(v);
		    }

		    // build a TokenDecl for the first variable with the call as initializer
		    // The multi-return unpack will be handled at compile time
		    TokenDecl *td = new TokenDecl(*vars[0]);
		    td->file = tb->file;
		    td->line = tb->line;
		    td->column = tb->column;

		    // store extra info for multi-return compile
		    // We use a TokenMultiReturn node that wraps the call and target variables
		    TokenAssign *assign = new TokenAssign();
		    assign->file = tb->file;
		    assign->line = tb->line;
		    assign->column = tb->column;
		    assign->left = new TokenVar(*vars[0]);
		    assign->right = rhs;
		    assign->multi_vars = vars; // store all target variables
		    td->initialize = assign;

		    DBG(std::cout << "parseStatement() multi-return ':=' with " << ids.size() << " variables" << std::endl);
		    return td;
		}

		// single := declaration (existing behavior)
		nextToken(); // consume :=
		TokenBase *rhs = parseExpression(nextToken());
		DataDef *inferred = rhs->datadef();
		if ( !inferred || inferred == &ddVOID )
		    inferred = &ddINT64;
		TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
		bool alloc = (!code) ? true : false;
		Variable *var = addVariable(code, *inferred, first_id, 1, NULL, alloc);
		TokenDecl *td = new TokenDecl(*var);
		td->file = tb->file;
		td->line = tb->line;
		td->column = tb->column;
		TokenAssign *assign = new TokenAssign();
		assign->file = tb->file;
		assign->line = tb->line;
		assign->column = tb->column;
		assign->left  = new TokenVar(*var);
		assign->right = rhs;
		td->initialize = assign;
		DBG(std::cout << "parseStatement() ':=' declared '" << first_id << "' type=" << inferred->name << std::endl);
		return td;
	    }
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	    // multi-return function declaration: (type, type) funcname(...)
	    if ( tb->id() == TokenID::tkOpBrk && peekToken()
		 && peekToken()->type() == TokenType::ttDataType )
	    {
		std::vector<DataDef *> rtypes;
		while ( true )
		{
		    TokenBase *rt = nextToken();
		    if ( rt->type() != TokenType::ttDataType )
			Throw(rt) << "Expecting type in multi-return declaration" << flush;
		    rtypes.push_back(&((TokenDataType *)rt)->definition);
		    TokenBase *sep = nextToken();
		    if ( sep->id() == TokenID::tkClBrk ) break;
		    if ( sep->id() != TokenID::tkComma )
			Throw(sep) << "Expecting , or ) in multi-return type list" << flush;
		}
		TokenBase *fname = nextToken();
		if ( fname->type() != TokenType::ttIdentifier )
		    Throw(fname) << "Expecting function name after multi-return type list" << flush;
		std::string id = ((TokenIdent *)fname)->str;
		TokenBase *opbrk = nextToken();
		if ( opbrk->id() != TokenID::tkOpBrk )
		    Throw(opbrk) << "Expecting ( after function name" << flush;
		parseFunction(*rtypes[0], id, NULL, &rtypes);
		return NULL;
	    }
	    DBG(std::cout << "parseStatement(" << (int)tb->type() << ") calling parseExpression" << std::endl);
	    resetPrevToken();
	    return parseExpression(tb);
	    break;

	case TokenType::ttKeyword:
	    // Container-type keywords (map, vector, set, list) are only types when
	    // followed by '<'. Otherwise the user is using them as an identifier —
	    // e.g. `MAP_DATA *map;` followed by `map->vnum = …;` — so route through
	    // parseExpression which already accepts them as contextual identifiers.
	    if ( (tb->id() == TokenID::tkMAP || tb->id() == TokenID::tkVECTOR
		|| tb->id() == TokenID::tkSET || tb->id() == TokenID::tkLIST)
		&& peekToken() && peekToken()->id() != TokenID::tkLT )
	    {
		DBG(std::cout << "parseStatement() container keyword used as identifier: "
		    << ((TokenKeyword *)tb)->str << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
	    // `class` is also a madc keyword (OOP class declaration), but C
	    // codebases (notably SMAUG) use it as a plain identifier for
	    // struct members / locals (`ch->class`, `int class;`). Treat as
	    // an identifier here when it's not the start of an actual class
	    // declaration — i.e. when the next token is not an identifier
	    // (class name) or '{' (anonymous class body).
	    if ( tb->id() == TokenID::tkCLASS
	      && peekToken()
	      && peekToken()->type() != TokenType::ttIdentifier
	      && peekToken()->id() != TokenID::tkOpBrc )
	    {
		DBG(std::cout << "parseStatement() 'class' used as identifier" << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
	    // `try` / `catch` / `throw` are C++ keywords but valid C
	    // identifiers (SMAUG has `int try;` then `try = saving_throw()`).
	    // A real try-block is `try { ... }`; a real catch starts a
	    // `catch (...)` block; a real throw is `throw expr;`. In any
	    // other follower context, treat as an identifier and route
	    // through parseExpression.
	    if ( (tb->id() == TokenID::tkTRY
	       || tb->id() == TokenID::tkCATCH
	       || tb->id() == TokenID::tkTHROW)
	      && peekToken()
	      && peekToken()->id() != TokenID::tkOpBrc
	      && peekToken()->id() != TokenID::tkOpBrk )
	    {
		DBG(std::cout << "parseStatement() '"
		    << ((TokenKeyword *)tb)->str << "' used as identifier" << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
	    DBG(std::cout << "parseKeyword(" << ((TokenKeyword *)tb)->str << ") calling parseKeyword" << std::endl);
	    return parseKeyword((TokenKeyword *)tb);

/* keep this here for tokentype reference
	case TokenType::ttBase:
	case TokenType::ttOperator:
	case TokenType::ttIdentifier:
	case TokenType::ttString:
	case TokenType::ttChar:
	case TokenType::ttInteger:
	case TokenType::ttReal:
	case TokenType::ttKeyword:
	case TokenType::ttDataType:
*/
	default:
	    Throw(tb) << "unexpected token type " << (int)tb->type() << flush;
    } // end switch
    DBG(cout << "parseStatement() returns NULL" << endl);

    return NULL;
}

// parse the token queue
bool Program::parse(TokenProgram *tp)
{
    TokenBase *tb, *ts;

    DBG(cout << endl << "Program::parse() START" << endl);

    if ( tokens.empty() )
    {
	cerr << "Program::parse() token queue empty" << endl;
	return false;
    }

    _parser_init();

    DBG(cout << endl << "Program::parse() calling ast.push for TokenProgram" << endl);
    ast.push(tp);

    try
    {
	while ( !tokens.empty() )
	{
	    tb = nextToken();
//	    printt(tb);
#if 1
	    ts = parseStatement(tb);
	    if ( ts )
	    {
		if ( ts->type() != TokenType::ttCompound )
		{
		    DBG(cout << "Program::parse() calling tp->statements.push_back" << endl);
		    tp->statements.push_back((TokenStmt *)ts);
		}
		else
		{
		    DBG(cout << "Program::parse() calling ast.push" << endl);
		    ast.push(ts);
		}
	    }
#endif
        }
    }
    catch(const char *err_msg)
    {
	cerr << ANSI_WHITE << tp->source << ':' << tb->line << ':' << tb->column 
	     << ": \e[1;31merror:\e[1;37m " << err_msg << ANSI_RESET << endl;
	source.showerror(tb->line, tb->column);
	return false;
    }
    catch(TokenIdent *ti)
    {
	cerr << ANSI_WHITE << tp->source << ':' << ti->line << ':' << ti->column
	     << ": \e[1;31merror:\e[1;37m use of undeclared identifier '" << ti->str << '\'' << ANSI_RESET << endl;
	source.showerror(ti->line, ti->column);
	return false;
    }
    catch(TokenBase *tb)
    {
	cerr << ANSI_WHITE << tp->source << ':' << tb->line << ':' << tb->column
	     << ": \e[1;31merror:\e[1;37m unexpected token type " << (int)tb->type() << ANSI_RESET << endl;
	source.showerror(tb->line, tb->column);
	if ( tb->type() == TokenType::ttReal )
	{
	    cerr << "TokenReal value: " << ((TokenReal *)tb)->dval() << endl;
	    printf("%.14lf\n", ((TokenReal *)tb)->dval());
	}
	return false;
    }
    catch(std::exception &e)
    {
	// throwbuf::sync() already printed the formatted error to stderr before throwing
	return false;
    }

    DBG(std::cout << "Program::parse() finished parsing" << std::endl);
    
    return true;
}
