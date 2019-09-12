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
#define DBG(x)
#include <asmjit/asmjit.h>
#include "tokens.h"
#include "datadef.h"
#include "madc.h"

using namespace std;
using namespace asmjit;

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
DataDefOSTREAM ddOSTREAM;
DataDefSSTREAM ddSSTREAM;
DataDefLPSTR ddLPSTR;


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

int TokenAssign::operate() const
{
    DBG(std::cout << "TokenAssign" << std::endl);
    if ( left->type() != TokenType::ttVariable )
    {
	std::cerr << "TokenAssign::operate() left side not variable" << std::endl;
	return 0;
    }
//  DBG(std::cout << "TokenAssign: " << ((TokenVar *)left)->var.name << "=" << right->val() << std::endl);
    DBG(std::cout << "TokenAssign: " << dynamic_cast<TokenVar *>(left)->var.name << "=" << right->val() << std::endl);
    left->set(right->val());
    return right->val();
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
	    if ( alloc && count == 1 && type->basetype() != BaseType::btFunct )
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


// add methods to ddSTRING
void Program::add_string_methods()
{
    string_member_cast scmc;
    Variable *var;

    scmc.c_str = (const char *(string::*)(void))&string::c_str;
    var = addFunction("c_str", datatype_vec_t{rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)(fnSTRINGcstr)scmc.void_pointer[0], true);
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

    DBG(std::cout << "add_string_methods() ddSTRING.methods.size() = " << ddSTRING.methods.size() << std::endl);
}


// some debugging functions
void printinteger(int i)
{
    std::cout << i << std::endl;
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

// needed to add getline
typedef istream& (*fnGETLINE)(istream&, string&);
// needed to add endl
typedef ostream& (*fnENDL)(ostream&);

// add system library functions
void Program::add_functions()
{
    addFunction("printstarred", datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)printstarred );
    addFunction("printstr",     datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)printstring);
    addFunction("printstream",  datatype_vec_t{DataType::dtVOID, DataType::dtSSTREAM}, (fVOIDFUNC)printstream);
    addFunction("puts",		datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)puts);
    addFunction("puti",		datatype_vec_t{DataType::dtVOID, DataType::dtINT}, (fVOIDFUNC)printinteger);
    addFunction("putchar",	datatype_vec_t{DataType::dtINT,  DataType::dtINT}, (fVOIDFUNC)putchar);
    addFunction("getline",	datatype_vec_t{rtPtr(DataType::dtISTREAM),rtPtr(DataType::dtISTREAM),rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnGETLINE)std::getline);
    addFunction("endl",		datatype_vec_t{rtPtr(DataType::dtOSTREAM),rtPtr(DataType::dtOSTREAM)}, (fVOIDFUNC)(fnENDL)std::endl);
}

// define some global variables
void Program::add_globals()
{
    addGlobal(ddSTRING,  "version", 1, (void *)"v0.0.1");
    addGlobal(ddOSTREAM, "cout", 1, std::cout.rdbuf());
    addGlobal(ddOSTREAM, "cerr", 1, std::cerr.rdbuf());
}

void Program::_parser_init()
{
    add_functions();
    add_string_methods();
    add_globals();
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
	return var;
    var = new Variable(id, dd, c, init, alloc);
    tkProgram->variables.push_back(var);

    DBG(std::cout << "Added new global variable type: " << dd.name << " size: "
		<< dd.size << " name: " << id << " ptr: " << var << " flags: " << var->flags << std::endl);
    DBG(std::cout << "Data address: " << (uint64_t)var->data << std::endl);

    return var;
}

// add a function definition
Variable *Program::addFunction(std::string id, datatype_vec_t params, fVOIDFUNC extfunc, bool isMethod)
{
    variable_map_iter vmi;
    funcdef_map_iter fmi;
    FuncDef *func;
    Variable *var;
    DataDef *dd;

    // may have already been declared
    if ( !isMethod && (fmi=funcdef_map.find(id)) != funcdef_map.end() )
    {
	DBG(std::cout << "addFunction() already declared: " << id << std::endl);
	return NULL;
    }

    switch(DataDef::rawtype(params[0]))
    {
	default:	 	  dd = &ddVOID;		break;
	case DataType::dtCHAR:    dd = &ddCHAR;		break;
	case DataType::dtUINT8:   dd = &ddUINT8;	break;
	case DataType::dtINT16:   dd = &ddINT16;	break;
	case DataType::dtUINT16:  dd = &ddUINT16;	break;
	case DataType::dtINT32:   dd = &ddINT32;	break;
	case DataType::dtUINT32:  dd = &ddUINT32;	break;
	case DataType::dtINT64:	  dd = &ddINT64;	break;
	case DataType::dtUINT64:  dd = &ddUINT64;	break;
	case DataType::dtSTRING:  dd = &ddSTRING;	break;
	case DataType::dtOSTREAM: dd = &ddOSTREAM;	break;
	case rtPtr(DataType::dtCHAR): dd = &ddLPSTR;	break;
    }

    func = new FuncDef(*dd);
    if ( !isMethod )
	funcdef_map[id] = func;
    DBG(std::cout << "addFunction() Added new function declaration name: " << id << " numparams: " << params.size()-1  << " x86code: " << (uint64_t)extfunc << std::endl);

    // func->parameters.push_back(&pb->definition);

    for ( uint32_t i = 1; i < params.size(); ++i )
    {
	switch(params[i]) // DataDef::rawtype(params[i])
	{
	    default:	 	      dd = &ddVOID;	break;
	    case DataType::dtCHAR:    dd = &ddCHAR;	break;
	    case DataType::dtUINT8:   dd = &ddUINT8;	break;
	    case DataType::dtINT16:   dd = &ddINT16;	break;
	    case DataType::dtUINT16:  dd = &ddUINT16;	break;
	    case DataType::dtINT32:   dd = &ddINT32;	break;
	    case DataType::dtUINT32:  dd = &ddUINT32;	break;
	    case DataType::dtINT64:   dd = &ddINT64;	break;
	    case DataType::dtUINT64:  dd = &ddUINT64;	break;
	    case DataType::dtSTRING:  dd = &ddSTRING;	break;
	    case DataType::dtOSTREAM: dd = &ddOSTREAM;  break;
	    case rtPtr(DataType::dtCHAR): dd = &ddLPSTR;break;
	}

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
			throw "Missing operand";
		    }
		    to->right = exStack.top(); exStack.pop(); DBG(cout << "popped " << to->right->val() << endl);
		}
		if ( to->argc() > 1 )
		{
		    if ( !to->left )
		    {
			if ( exStack.empty() )
			{
			    DBG(cerr << "got operator, but exStack is empty!" << endl);
			    throw "Missing operand";
			}
			to->left = exStack.top(); exStack.pop();  DBG(cout << "popped " << to->left->val() << endl);
		    }
		}
	    }
	    DBG(cout << "Popping " << (char)to->get() << "[" << (to->left ? to->left->val() : 0) << ", " << (to->right ? to->right->val() : 0) << "] from opStack and onto exStack" << endl);
	    opStack.pop();
	    exStack.push(to);
	    break;
	case TokenType::ttCallFunc:
	    DBG(cout << "popOperator() got ttCallFunc" << endl);
	    exStack.push(opStack.top());
	    opStack.pop();
	    break;
	default:
	    DBG(cerr << "popOperator() throwing opStack.top()" << endl);
	    throw opStack.top();
    } // end switch
    DBG(cout << "popOperator() size: " << opStack.size() << " END" << endl);
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
    int paramcnt = 0;

    while ( brackets )
    {
	tb = peekToken();
	if ( tb->id() == TokenID::tkSemi )  { return tb; }
	tb = nextToken();
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	if ( tb->id() == TokenID::tkComma )
	{
	    if ( ++paramcnt >= ((FuncDef *)tc->var.type)->parameters.size() )
		throw "Too many parameters";
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
    if ( tc->argc() != ((FuncDef *)tc->var.type)->parameters.size() )
    {
	DBG(std::cout << "parseCallFunc: argument count: " << tc->argc() << " expected: " << ((FuncDef *)tc->var.type)->parameters.size() << std::endl);
	throw "Incorrect number of parameters";
    }

    return tb;
}

// parse one complete expression
// for expression: x = 5, sum(5, 5), ++x, etc
// a "conditional" expression stops when brackets are equalized
TokenBase *Program::parseExpression(TokenBase *tb, bool conditional)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenOperator *to;
    stack<TokenBase *> exStack;
    stack<TokenBase *> opStack;
    TokenBaseType *bt;
    Variable *var;
    bool done;
    int brackets = 0;

    DBG(std::cout << tb->line << ':' << tb->column << ":Program::parseExpression(" << tb->get() << " type: " << (int)tb->type() << ") start" << (conditional ? " conditional" : "") << std::endl);

//    for ( done = false; !done && tb; tb = peekToken() )
    while ( !done && tb )
    {
parseexpswitchtop:
	switch(tb->type())
	{
	    case TokenType::ttInteger:
	        DBG(cout << "Pushing number: " << (int)tb->get() << " onto exStack" << endl);
		exStack.push(tb); // exStack.push(tb->clone());
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
		if ( tb->id() == TokenID::tkOpBrk )
		{
		    ++brackets;
		    DBG(cout << "Got (, pushing onto opStack" << endl);
		    opStack.push(tb); // opStack.push(tb->clone());
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
			popOperator(opStack, exStack);
		    if ( !opStack.empty() )
			opStack.pop(); // pop off '('
		    if ( conditional && !brackets )
		    {
			DBG(std::cout << "Program::parseExpression() conditional end exStack:" << exStack.size() << std::endl);
			return exStack.empty() ? NULL : exStack.top();
		    }
		    break;
		}
		// see if we need to convert TokenNeg to TokenSub
		if ( tb->id() == TokenID::tkNeg && prevToken()
		&&  (prevToken()->id() == TokenID::tkClBrk || !prevToken()->is_operator()) )
		{
		    DBG(std::cout << "parseExpression() converting TokenNeg to TokenSub, prevToken id: " << (int)prevToken()->id() << " prevToken->is_operator: " << (prevToken()->is_operator() ? "true" : "false") << std::endl);
		    TokenSub *ts = new TokenSub();

		    ts->file = tb->file;
		    ts->line = tb->line;
		    ts->column = tb->column;
		    // should we delete tb ?
		    tb = ts;
		}
		if ( tb->id() == TokenID::tkDec || tb->id() == TokenID::tkInc )
		{
		    DBG(cout << "parseExpression: Got operator: " << (char)tb->get() << (char)tb->get() << endl);
		    if ( exStack.empty() )
		    {
			to = (TokenOperator *)tb; // ->clone();
			opStack.push(to);
		    }
		    else
		    {
			to = (TokenOperator *)tb; // ->clone();
			to->left = exStack.top(); exStack.pop(); DBG(cout << "popped " << to->left->val() << endl);
			exStack.push(to);
		    }
		    break;
		}
		DBG(cout << "parseExpression: Got operator: " << (char)tb->get() << " id() " << (int)tb->id() << endl);
		to = (TokenOperator *)tb; // ->clone();
		// whiile: there is a function at the top of the operator stack)
		// or (there is an operator at the top of the operator stack with greater precedence)
		// or (the operator at the top of the operator stack has equal precedence and is left associative))
		// and (the operator at the top of the operator stack is not a left parenthesis):
		// (Note: we don't put functions in the stack right now)
		while ( !opStack.empty() && opStack.top()->id() != TokenID::tkOpBrk
		&&      (opStack.top()->type() == TokenType::ttCallFunc
		||      (opStack.top()->is_operator() && (*((TokenOperator *)opStack.top()) > *to))) )
		{
		    DBG(cout << "Operator(" << (char)opStack.top()->get() << ") has precedence over operator(" << (char)to->get() << ')' << endl);
		    popOperator(opStack, exStack);
		}
		DBG(cout << "Pushing " << (char)tb->get() << " onto opStack" << endl);
		opStack.push(to);
		break;
            case TokenType::ttBaseType:
		bt = (TokenBaseType *)tb;
		tb = nextToken();
		if ( tb->type() != TokenType::ttIdentifier ) { throw "Expecting identifier"; }
		var = addVariable(code, bt->definition, ((TokenIdent *)tb)->str);
		DBG(cout << "Pushing newly declared variable: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttString:
		var = addLiteral(((TokenIdent *)tb)->str);
		DBG(cout << "Pushing new variable of literal: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttIdentifier:
		if ( tb->type() == TokenType::ttIdentifier
		&& !(var=findVariable(((TokenIdent *)tb)->str)) )
		{
		    DBG(cerr << "parseExpression() failed to resolve identifier " << ((TokenIdent *)tb)->str << endl);
		    throw (TokenIdent *)tb;
		}
#if 1
		if ( var->type->is_function() )
		{
		    TokenCallFunc *tc = new TokenCallFunc(*var);
		    tb = nextToken();
		    tc->line = tb->line;
		    tc->column = tb->column;
		    // if bracket, parse params
		    if ( tb->id() == TokenID::tkOpBrk )
		    {
			// delete tb?
			tb = parseCallFunc(tc);
			DBG(cout << "parseCallFunc returned with token " << (char)tb->get() << endl);
		    }
		    DBG(cout << "Pushing found function call: " << var->name << "() onto opStack" << endl);
		    opStack.push(tc);
		    // I'm not sure why I need to do this TODO: figure this out
		    if ( tb->id() == TokenID::tkSemi )
			done = true;
		    break;
		}
#else
		if ( var->type->is_function() )
		{
		    if ( peekToken()->id() != TokenID::tkOpBrk )
			throw "Expecting (";
		    tb = nextToken();
		    TokenCallFunc *tc = new TokenCallFunc(*var);
		    tc->line = tb->line;
		    tc->column = tb->column;
		    // delete tb?
		    tb = parseCallFunc(tc);
		    DBG(cout << "Pushing found function call: " << var->name << "() onto opStack" << endl);
		    opStack.push(tc);
		    DBG(cout << "parseCallFunc returned with token " << (char)tb->get() << endl);
		    // I'm not sure why I need to do this TODO: figure this out
		    if ( tb->id() == TokenID::tkSemi )
			done = true;
		    break;
		}
#endif
		if ( var->type->is_numeric() )
		    DBG(cout << "Pushing found variable: " << var->name << '=' << (int)var->get<int>() << " onto exStack" << endl);
		else
		    DBG(cout << "Pushing found variable: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttVariable:
		var = &dynamic_cast<TokenVar *>(tb)->var;
		if ( var->type->is_numeric() )
		    DBG(cout << "Pushing direct variable: " << var->name << '=' << (int)var->get<int>() << " onto exStack" << endl);
		else
		    DBG(cout << "Pushing direct variable: " << var->name << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    case TokenType::ttFunction:
		throw "Got function!";
		break;
	    case TokenType::ttCallFunc:
		throw "Got call function!";
		break;
	    case TokenType::ttChar:
	        DBG(cout << "Pushing char: " << (int)tb->get() << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    default:
		DBG(std::cerr << "parseExpression() primary switch throwing token" << std::endl);
		throw tb;
	}
	if ( done ) { break; /* prevent eating next token */ }
	tb = peekToken();
	if ( tb->id() == TokenID::tkClBrk && !brackets )
	{
	    DBG(cout << "Hit ), no prior brackets, might be end of function?" << endl);
	    break;
	}
	tb = nextToken();
    }

    if ( !opStack.empty() )
	DBG(cout << "Emptying operator stack" << endl);
    while ( !opStack.empty() )
    {
	popOperator(opStack, exStack);
#if 0
	tb = opStack.top();
	if ( tb->type() == TokenType::ttCallFunc )
	{
//	    throw "Got call function :)";
	    DBG(cout << "parseExpression() got call function :)" << endl);
	}
	exStack.push(tb);
	opStack.pop();
#endif
    }
    DBG(cout << "parseExpression() exStack size: " << exStack.size() << endl);
    DBG(if ( !exStack.empty() ) std::cout << " exStack.top()->type() = " << (int)exStack.top()->type() << endl);

    DBG(std::cout << "Program::parseExpression() end" << std::endl);

    return exStack.empty() ? NULL : exStack.top();
}


TokenRETURN *TokenRETURN::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenRETURN::parse()" << std::endl);
    tn = pgm.nextToken();

    // return with no value, should check if function has a return value
    if ( tn->id() == TokenID::tkSemi )
    {
	return this;
    }
    returns = pgm.parseExpression(tn);

    return this;
}

TokenIF *TokenIF::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenIF::parse()" << std::endl);
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenIF::parse() expecting (" << endl);
	throw tn;
    }
    DBG(cout << "TokenIF::parse() calling condition=parseExpression(" << (char)tn->get() << ')' << endl);
    if ( !(condition=pgm.parseExpression(tn, true)) )
	throw "Failed to parse if expression";

    tn = pgm.nextToken();
    DBG(cout << "TokenIF::parse() calling statement=parseStatement(" << (char)tn->get() << ')' << endl);
    if ( !(statement=pgm.parseStatement(tn)) )
	throw "Failed to parse if statement";

    tn = pgm.peekToken();
    if ( tn && tn->id() == TokenID::tkELSE )
    {
	tn = pgm.nextToken(); // get the else
	tn = pgm.nextToken(); // skip the else
	DBG(cout << "TokenIF::parse() calling elsestmt=parseStatement(" << (char)tn->get() << ')' << endl);
	elsestmt = pgm.parseStatement(tn);
	if ( !elsestmt )
	    throw "parse error on else";
    }
    else
    if ( tn )
	DBG(cout << "TokenIF::peekToken() type: " << (int)tn->type() << " id: " << (int)tn->id() << ')' << endl);

    return this;
}

TokenFOR *TokenFOR::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenFOR::parse() START" << std::endl);
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenFOR::parse() expecting (" << endl);
	throw tn;
    }

    tn = pgm.nextToken();
    DBG(cout << "TokenFOR::parse() initialize: calling parseStatement(" << (char)tn->get() << ')' << endl);
    if ( !(initialize = pgm.parseStatement(tn)) )
	throw "Failed to parse initialize";
    tn = pgm.nextToken();
    DBG(cout << "TokenFOR::parse() condition: calling parseExpression(" << (char)tn->get() << ')' << endl);
    if ( !(condition = pgm.parseExpression(tn, true)) )
	throw "Failed to parse expression";

    tn = pgm.nextToken();
    DBG(cout << "TokenFOR::parse() increment: calling parseStatement(" << (char)tn->get() << ')' << endl);
    if ( !(increment = pgm.parseStatement(tn)) )
	throw "Failed to parse increment";

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkClBrk )
	throw "Expecting )";

    tn = pgm.nextToken();

    DBG(cout << "TokenFOR::parse() statement(s): calling parseStatement(" << (char)tn->get() << ')' << endl);
    if ( !(statement = pgm.parseStatement(tn)) )
	throw "Failed to parse statement";

    DBG(std::cout << "TokenFOR::parse() END" << std::endl);

    return this;
}

TokenWHILE *TokenWHILE::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenWHILE::parse()" << std::endl);
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenWHILE::parse() expecting (" << endl);
	throw tn;
    }
    DBG(cout << "TokenWHILE::parse() calling parseExpression(" << (char)tn->get() << ')' << endl);
    condition = pgm.parseExpression(tn, true);

    tn = pgm.nextToken();
    DBG(cout << "TokenWHILE::parse() calling parseStatement(" << (char)tn->get() << ')' << endl);
    statement = pgm.parseStatement(tn);

    return this;
}

TokenDO *TokenDO::parse(Program &pgm)
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
	throw "Expecting while";
    }
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenDO::parse() expecting (" << endl);
	throw "Expecting (";
    }
    DBG(cout << "TokenDO::parse() calling parseExpression(" << (char)tn->get() << ')' << endl);
    condition = pgm.parseExpression(tn, true);

    return this;
}

// parse operator overload
TokenOPEROVER *TokenOPEROVER::parse(Program &pgm)
{
    TokenBase *tn;
    DBG(std::cout << std::endl << "TokenOPEROVER::parse()" << std::endl);
    tn = pgm.nextToken();
    // overload type conversion
    if ( tn->type() == TokenType::ttBaseType )
    {
    }
    // overloading operator
    switch(tn->id())
    {
	// multi-token
	case TokenID::tkOpBrk:
	    if ( pgm.peekToken()->id() != TokenID::tkClBrk )
		throw "Expecting )";
	    delete tn;
	    delete pgm.nextToken();
	    str = "()";
	    return this;
	case TokenID::tkOpSqr:
	    if ( pgm.peekToken()->id() != TokenID::tkClSqr )
		throw "Expecting ]";
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
	    throw tn;
    }
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
void Program::parseFunction(DataDef &dd, std::string &id)
{
    variable_map_iter vmi;
    funcdef_map_iter fmi;
    datadef_vec_iter dvi;
    FuncDef *func;
    TokenBase *nt = NULL; // next token;
    Variable *var;

    vector<std::string> ids;  // vector of variable names
    TokenBaseType *pb;        // parameter basetype
    std::string pid;          // parameter id
    RefType rtype = RefType::rtNone;

    DBG(cout << "parseFunction(" << dd.name << ' ' << id << ") START" << endl);

    // may have already been declared
    if ( (fmi=funcdef_map.find(id)) != funcdef_map.end() )
	func = fmi->second;
    else
    {
	func = new FuncDef(dd);
	funcdef_map[id] = func;
	DBG(std::cout << "parseFunction() Added new function declaration type: " << dd.name << " size: " << dd.size << " name: " << id << std::endl);
    }

    // look for parameters
    while ( (nt=nextToken()) && nt->id() != TokenID::tkClBrk )
    {
	if ( nt->type() != TokenType::ttBaseType )
	{
	    DBG(std::cerr << "parseFunction() params: failed to obtain basetype" << std::endl);
	    throw "Failed to find type when parsing function parameters";
	}
	pb = (TokenBaseType *)nt;
	rtype = RefType::rtValue;
grabnt:
	// grab the next token
	if ( !peekToken() )
	    throw "Unexpected end of file parsing function parameters";

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
	    DBG(std::cout << "parseFunction() ignoring pointer token" << std::endl);
	    goto grabnt;
	}
	if ( nt->type() != TokenType::ttIdentifier )
	{
	    throw "Expecting identifier after type";
	}

	// grab identifier string
	pid = ((TokenIdent *)nt)->str;
	if ( !peekToken() )
	    throw "Expecting token after identifier";

	nt = nextToken();

	// parameter declaration
	if ( nt->id() == TokenID::tkComma || nt->id() == TokenID::tkClBrk )
	{
	    if ( !func->findParameter(pid) )
	    {
		ids.push_back(pid);
		if ( rtype == RefType::rtReference && pb->definition.rawtype() == DataType::dtSTRING )
		    func->parameters.push_back(&ddSTRINGref);
		else
		    func->parameters.push_back(&pb->definition);
		DBG(std::cout << "Added new parameter declaration type: " << dd.name << " size: "
		    << dd.size << " name: " << pid << " ptr: " << var << std::endl);
	    }
	    else
	    {
		DBG(std::cerr << "parseFunction() params: duplicate parameter name " << pid << std::endl);
		throw "Duplicate parameter name";
	    }
	    if ( nt->id() == TokenID::tkClBrk )
		break;
	}
    }

    if ( !nt )
    {
	DBG(std::cerr << "parseFunction() expecting more tokens, missing closing bracket" << std::endl);
	throw "Missing closing bracket";
    }

    nt = nextToken();

    Method *method;

    if ( (var=tkProgram->findVariable(id)) )
    {
	// should make sure type is correct
	method = (Method *)var->data;
    }
    else
    {
	var = addVariable(NULL, *func, id);
	method = new Method(*var);
	var->data = (void *)method;
    }

    // semicolon means this is just a function declaration
    if ( nt->id() == TokenID::tkSemi )
    {
	DBG(std::cout << "parseFunction() forward declaration of function " << id << std::endl);
	return;
    }

    // need to see a brace to define a function
    if ( nt->id() != TokenID::tkOpBrc )
    {
	// throw error
	throw "Expecting brace after function declaration";
    }

    DataDef *d;
    Variable *v;
    int i = 0;

    for ( dvi = func->parameters.begin(); dvi != func->parameters.end(); ++dvi )
    {
	d = *dvi;
	DBG(cout << "parseFunction() adding parameter variable " << ids[i] << endl);
	v = new Variable(ids[i++], *d, 1, NULL, false);
	method->parameters.push_back(v);
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

    DBG(cout << "parseFunction() calling ast.push" << endl);
    ast.push(tf);

    DBG(cout << "parseFunction(" << id << ") END" << endl);
}

// parse either a variable declaration, or a function declaration
TokenBase *Program::parseDeclaration(TokenBaseType *tb)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenBase *nt; // next token;
    Variable *var;
    string id;
    bool gotstatic = false;

    DBG(std::cout << "parseDeclaration(" << tb->str << ") START " << (tb->file ? tb->file : "NULL") << ':' << tb->line << ':' << tb->column << std::endl);

    if ( !peekToken() )
	throw "Unexpected end of data: Expecting identifier after type";
    nt = nextToken();

    if ( nt->type() != TokenType::ttIdentifier )
    {
	DBG(cerr << "parseDeclaration() nt->type()=" << (int)nt->type() << endl);
	throw "Expecting identifier after type";
    }
    // grab identifier string
    id = ((TokenIdent *)nt)->str;
    DBG(std::cout << "parseDeclaration() identifier: " << id << std::endl);

    if ( !(nt=peekToken()) )
	throw "expecting token after identifier";

    // variable declaration
    if ( nt->id() == TokenID::tkSemi || nt->id() == TokenID::tkAssign )
    {
	bool alloc = (!code || gotstatic) ? true : false;
	var = addVariable(code, tb->definition, id, 1, NULL, alloc);
	TokenDecl *td = new TokenDecl(*var);

	td->file = tb->file;
	td->line = tb->line;
	td->column = tb->column;

	if ( nt->id() == TokenID::tkAssign )
	{
	    DBG(std::cout << "parseDeclaration() calling td->initialize = parseExpression" << std::endl);
	    td->initialize = parseExpression(new TokenVar(*var));
	}
	DBG(std::cout << "parseDeclaration() returning" << std::endl);

	return td;
    }

    nt = nextToken();
    if ( nt->id() != TokenID::tkOpBrk )
    {
	DBG(std::cerr << "parseDeclaration() throwing token " << (int)nt->id() << std::endl);
	throw nt;
    }

    DBG(std::cout << "parseDeclaration() returning" << std::endl);

    parseFunction(tb->definition, id);

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
	case TokenType::ttBaseType:
	    DBG(std::cout << "parseStatement(" << (int)tb->type() << ") calling parseDeclaration" << std::endl);
	    return parseDeclaration((TokenBaseType *)tb);
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
	    throw tb;

	// if we start with an operator or an identifier, then this could be an
	// assignment or a function call
	case TokenType::ttIdentifier:
	    DBG(std::cout << "parseStatement() got identifier " << ((TokenIdent *)tb)->str << std::endl);
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	    DBG(std::cout << "parseStatement(" << (int)tb->type() << ") calling parseExpression" << std::endl);
	    return parseExpression(tb);
	    break;

	case TokenType::ttKeyword:
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
	case TokenType::ttBaseType:
*/
	default:
	    throw tb;
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
	cerr << ANSI_WHITE << tp->source << ':' << _line << ':' << _column 
	     << ": \e[1;31merror:\e[1;37m " << err_msg << ANSI_RESET << endl;
	showerror(*tp->is);
	return false;
    }
    catch(TokenIdent *ti)
    {
	cerr << ANSI_WHITE << tp->source << ':' << _line << ':' << _column
	     << ": \e[1;31merror:\e[1;37m use of undeclared identifier '" << ti->str << '\'' << ANSI_RESET << endl;
	showerror(*tp->is);
	return false;
    }
    catch(TokenBase *tb)
    {
	cerr << ANSI_WHITE << tp->source << ':' << _line << ':' << _column
	     << ": \e[1;31merror:\e[1;37m unexpected token type " << (int)tb->type() << ANSI_RESET << endl;
	showerror(*tp->is);
	return false;
    }

    DBG(std::cout << "Program::parse() finished parsing" << std::endl);
    
    return true;
}

