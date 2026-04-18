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
	return var;
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

    // may have already been declared
    if ( !isMethod && (fmi=funcdef_map.find(id)) != funcdef_map.end() )
    {
	DBG(std::cout << "addFunction() already declared: " << id << std::endl);
	return NULL;
    }

//  switch(DataDef::rawtype(params[0]))
    switch(params[0])
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
	case DataType::dtARRAY:   dd = &ddARRAY;	break;
	case rtPtr(DataType::dtOSTREAM):
	case DataType::dtOSTREAM: dd = &ddOSTREAM;	break;
	case rtPtr(DataType::dtCHAR): dd = &ddLPSTR;	break;
    }

    func = new FuncDef(*dd);
    if ( !isMethod )
	funcdef_map[id] = func;
    DBG(std::cout << "addFunction() Added new function declaration name: " << id << " numparams: " << params.size()-1  << " x86code: " << (uint64_t)extfunc << " returns " << dd->name << std::endl);

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
	    case DataType::dtARRAY:   dd = &ddARRAY;	break;
	    case DataType::dtOSTREAM: dd = &ddOSTREAM;  break;
	    case DataType::dtFLOAT:   dd = &ddFLOAT;    break;
	    case DataType::dtDOUBLE:  dd = &ddDOUBLE;   break;
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
TokenBase *Program::parseExpression(TokenBase *tb, bool conditional)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenOperator *to;
    stack<TokenBase *> exStack;
    stack<TokenBase *> opStack;
    TokenDataType *bt;
    Variable *var;
    bool done = false;
    int brackets = 0;

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
			// must have closing )
			if ( peekToken() && peekToken()->id() == TokenID::tkClBrk )
			{
			    nextToken(); // consume )
			    // parse the expression being cast
			    TokenBase *cast_expr_tb = nextToken();
			    TokenBase *cast_expr = parseExpression(cast_expr_tb, true);
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
		    if ( !exStack.empty()
		      && exStack.top()->type() == TokenType::ttMember
		      && dynamic_cast<DataDefFPTR *>(dynamic_cast<TokenMember *>(exStack.top())->var.type) )
		    {
			TokenMember *tmem = dynamic_cast<TokenMember *>(exStack.top());
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
			DBG(std::cout << "Program::parseExpression() conditional end exStack:" << exStack.size() << std::endl);
			return exStack.empty() ? NULL : exStack.top();
		    }
		    break;
		}
		// ternary operator: condition ? true_expr : false_expr
		if ( tb->id() == TokenID::tkTerQ )
		{
		    DBG(cout << "parseExpression: ternary operator ?" << endl);
		    // pop operators with higher or equal precedence than ? (13)
		    // but NOT assignment (14) or lower precedence operators
		    while ( !opStack.empty() && opStack.top()->get() != '('
			    && dynamic_cast<TokenOperator *>(opStack.top())
			    && dynamic_cast<TokenOperator *>(opStack.top())->precedence() <= 13 )
			popOperator(opStack, exStack);
		    if ( exStack.empty() )
			Throw(tb) << "Missing condition before ?" << flush;
		    TokenTerQ *ternary = (TokenTerQ *)tb;
		    ternary->condition = exStack.top();
		    exStack.pop();
		    // parse true expression — use conditional mode so it stops at : or )
		    // but : is an operator, not ), so we parse then check for :
		    TokenBase *texpr = nextToken();
		    ternary->true_expr = parseExpression(texpr, true);
		    // after conditional parseExpression, expect : next
		    TokenBase *colon = nextToken();
		    if ( colon->id() != TokenID::tkTerC )
			Throw(colon) << "Expecting : in ternary expression" << flush;
		    // parse false expression
		    TokenBase *fexpr = nextToken();
		    ternary->false_expr = parseExpression(fexpr, conditional);
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
		    if ( addr_tb->type() != TokenType::ttIdentifier )
			Throw(addr_tb) << "expecting variable name after '&'" << flush;
		    std::string aname = ((TokenIdent *)addr_tb)->str;
		    Variable *avar = findVariable(aname);
		    if ( !avar )
			Throw(addr_tb) << "undeclared identifier '" << aname << "'" << flush;
		    avar->flags |= vfADDRTAKEN;
		    if ( paren )
		    {
			TokenBase *close = nextToken();
			if ( !close || close->id() != TokenID::tkClBrk )
			    Throw(close ? close : addr_tb) << "expected ')' after &(name)" << flush;
		    }
		    DataDefPTR *aptr = getPointerType(avar->type);
		    exStack.push(new TokenAddrOf(*avar, aptr));
		    break;
		}
		// * dereference in unary position
		if ( tb->id() == TokenID::tkMul && isUnaryPosition() )
		{
		    TokenBase *deref_tb = nextToken();
		    if ( deref_tb->type() != TokenType::ttIdentifier )
			Throw(deref_tb) << "expecting variable name after '*'" << flush;
		    std::string dname = ((TokenIdent *)deref_tb)->str;
		    Variable *dvar = findVariable(dname);
		    if ( !dvar )
			Throw(deref_tb) << "undeclared identifier '" << dname << "'" << flush;
		    if ( !dvar->type->is_pointer() )
			Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
		    DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dvar->type);
		    DataDef *base = dptr ? dptr->base_type : &ddINT64;
		    exStack.push(new TokenDeref(*dvar, base));
		    break;
		}
		if ( tb->id() == TokenID::tkDec || tb->id() == TokenID::tkInc )
		{
		    DBG(cout << "parseExpression: Got operator: " << (char)tb->get() << (char)tb->get() << endl);
		    if ( isPostfixPosition() && !exStack.empty() )
		    {
			to = (TokenOperator *)tb;
			to->left = exStack.top(); exStack.pop(); DBG(cout << "popped " << to->left->ival() << endl);
			exStack.push(to);
		    }
		    else
		    {
			to = (TokenOperator *)tb;
			opStack.push(to);
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
	    case TokenType::ttIdentifier:
		// sizeof(type) — resolve to integer constant at parse time
		if ( ((TokenIdent *)tb)->str == "sizeof" )
		{
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
			if ( var )
			{
			    sizeof_value = var->type->size;
			    if ( var->is_fixed_array() )
				sizeof_value *= var->total_elements();
			}
			// check struct_map
			if ( !var )
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
		    if ( !var && !dd )
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
		    if ( !var )
			sizeof_value = dd->size;
		    exStack.push(new TokenInt((int)sizeof_value));
		    break;
		}
		// va_arg(ap, type) — compiler intrinsic for reading variadic args
		if ( ((TokenIdent *)tb)->str == "va_arg" )
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
			tv_var      = &tm->var;
			struct_type =  tm->var.type;
		    }
		    else
		    {
			TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(lhs_dot);
			tv_var      = &tsub->object;
			struct_type =  tsub->datadef(); // element type
		    }
		    if ( !struct_type->is_struct() && !struct_type->is_object() )
			Throw(tb) << "member reference is not a structure or union" << flush;
		    var = NULL;
		    string id = ((TokenIdent *)tb)->str;
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
		    if ( lhs_dot->type() == TokenType::ttMember
		      || lhs_dot->type() == TokenType::ttSubscript )
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

		    // get the pointer variable — from TokenVar or TokenMember (chained ->)
		    TokenBase *lhs = exStack.top();
		    Variable *obj_var = NULL;
		    DataDef *obj_type = NULL;
		    if ( lhs->type() == TokenType::ttVariable )
		    {
			obj_var = &dynamic_cast<TokenVar *>(lhs)->var;
			obj_type = obj_var->type;
		    }
		    else if ( lhs->type() == TokenType::ttMember )
		    {
			TokenMember *tm = dynamic_cast<TokenMember *>(lhs);
			obj_var = &tm->var;
			obj_type = tm->var.type;
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

		    string id = ((TokenIdent *)tb)->str;

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
		    if ( lhs->type() == TokenType::ttMember )
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
		    std::string ns_name = ((TokenIdent *)tb)->str;
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
			variable_map_iter vmi = nsi->second.find(((TokenIdent *)tb)->str);
			if ( vmi != nsi->second.end() )
			    var = vmi->second;
		    }
		}
		if ( !var )
		    var = findVariable(((TokenIdent *)tb)->str);
		// class method: resolve unqualified member name through __this
		if ( !var && code && code->method && code->method->owner_class )
		{
		    DataDefCLASS *cls = code->method->owner_class;
		    std::string mname = ((TokenIdent *)tb)->str;
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
		    var = lazy_resolve(((TokenIdent *)tb)->str);
		if ( !var && peekToken() && peekToken()->id() == TokenID::tkOpBrk )
		{
		    // dlsym fallback: try to resolve as a libc/system function
		    std::string fname = ((TokenIdent *)tb)->str;
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
		    DBG(cerr << "parseExpression() failed to resolve identifier " << ((TokenIdent *)tb)->str << endl);
		    Throw(tb) << "use of undeclared identifier '" << ((TokenIdent *)tb)->str << '\'' << flush;
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
			 || peek_id == TokenID::tkTerC;
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

	// forward typedef: typedef struct tag_name alias; (struct not yet defined)
	if ( dmi == pgm.struct_map.end() && do_typedef )
	{
	    // create placeholder struct (size 0, no members) for forward declaration
	    DataDefSTRUCT *fwd = new DataDefSTRUCT(tag->str, 0);
	    pgm.struct_map[tag->str] = fwd;
	    dmi = pgm.struct_map.find(tag->str);
	    DBG(cout << "TokenSTRUCT::parse() forward declaration of struct " << tag->str << endl);
	}
	if ( dmi == pgm.struct_map.end() )
	    pgm.Throw(tn) << "Unknown struct type '" << tag->str << "'" << flush;
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
	    TokenBase *stag = pgm.nextToken();
	    if ( stag->type() != TokenType::ttIdentifier )
		pgm.Throw(stag) << "Expecting struct name" << flush;
	    std::string sname = ((TokenIdent *)stag)->str;
	    datadef_map_iter sdmi = pgm.struct_map.find(sname);
	    if ( sdmi == pgm.struct_map.end() )
		pgm.Throw(stag) << "Unknown struct type '" << sname << "'" << flush;
	    mtype = new TokenDataType(sname.c_str(), *sdmi->second);
	}
	else
	    pgm.Throw(tn) << "Expecting type in struct definition" << flush;

	// check for pointer declarator(s): type * [*...] member_name
	DataDef *member_dd = &mtype->definition;
	// Function-pointer typedefs (DataDefFPTR) already represent pointers;
	// `DO_FUN *fn` inside a struct names the same storage as `DO_FUN fn`.
	bool mem_fnptr_base = (dynamic_cast<DataDefFPTR *>(member_dd) != NULL);
	while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
	{
	    pgm.nextToken(); // consume '*'
	    if ( !mem_fnptr_base )
		member_dd = pgm.getPointerType(member_dd);
	}

	// expect member name
	tn = pgm.nextToken();
	if ( tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting member name in struct definition" << flush;
	std::string mname = ((TokenIdent *)tn)->str;

	dds->addMember(mname, *member_dd, 1);
	DBG(cout << "TokenSTRUCT::parse() added member " << member_dd->name << ' ' << mname
	    << " (size " << member_dd->size << ", total " << dds->size << ')' << endl);

	// expect semicolon
	tn = pgm.nextToken();
	if ( tn->id() != TokenID::tkSemi )
	    pgm.Throw(tn) << "Expecting ';' after struct member" << flush;
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
    if ( tn && tn->type() == TokenType::ttIdentifier )
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

    // Multi-return detection: parseExpression consumed either `,` or `;`.
    // If it stopped on `,`, the next token is the second expression (an
    // identifier, number, string, `(`, unary op etc). If it stopped on
    // `;`, the next token is whatever the next statement starts with —
    // which is a keyword (if/while/return/...), `}` (block close), a type
    // name, or another identifier. Restrict detection to clear expression
    // starters so `return X;` followed by `if(...)` / `<ident>()` / etc.
    // doesn't misfire as multi-return.
    auto looks_like_second_return = [&]() -> bool {
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

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenIF::parse() expecting (" << endl);
	pgm.Throw(tn) << "expecting ( after if" << flush;
    }
    DBG(cout << "TokenIF::parse() calling condition=parseExpression(" << (char)tn->get() << ')' << endl);
    if ( !(condition=pgm.parseExpression(tn, true)) )
	pgm.Throw(tn) << "Failed to parse if expression" << flush;

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
	TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
	std::string id = ((TokenIdent *)tn2)->str;
	Variable *var = pgm.addVariable(code, dt->definition, id, 1, NULL, false);
	TokenDecl *td = new TokenDecl(*var);
	td->file = dt->file;
	td->line = dt->line;
	td->column = dt->column;

	TokenBase *tn_peek = pgm.peekToken();
	if ( tn_peek && tn_peek->id() == TokenID::tkAssign )
	{
	    // conditional=true so `;` stops without being consumed, matching
	    // the non-typed init path and letting the common ';' gate below
	    // work uniformly for both.
	    td->initialize = pgm.parseExpression(new TokenVar(*var), true);
	}
	initialize = td;
    }
    else
    {
	DBG(cout << "TokenFOR::parse() initialize: calling parseExpression(" << (char)tn->get() << ')' << endl);
	// conditional=true so parseExpression stops on `;` WITHOUT consuming;
	// comma continuations read below get to see `,` left in the stream.
	if ( !(initialize = pgm.parseExpression(tn, true)) )
	    pgm.Throw(tn) << "Failed to parse initialize" << flush;
    }

    // C comma-expression init: `for (a=0, b=1, ... ; ...)`. parseExpression
    // consumes `,`, so after a comma-terminated init the peek is already
    // the next expression starter; accept either case.
    while ( pgm.peekToken() )
    {
	TokenBase *pk = pgm.peekToken();
	if ( pk->id() == TokenID::tkComma ) { pgm.nextToken(); pk = pgm.peekToken(); }
	if ( !pk || pk->id() == TokenID::tkSemi ) break;
	// anything else must look like an expression starter
	if ( pk->type() == TokenType::ttSymbol || pk->type() == TokenType::ttKeyword
	  || pk->type() == TokenType::ttDataType ) break;
	tn = pgm.nextToken();
	TokenBase *extra = pgm.parseExpression(tn, true);
	if ( extra ) init_extras.push_back(extra);
    }

    tn = pgm.nextToken(); // consume `;` after init
    if ( tn->id() != TokenID::tkSemi )
	pgm.Throw(tn) << "Expecting ';' after for init" << flush;

    tn = pgm.nextToken();
    DBG(cout << "TokenFOR::parse() condition: calling parseExpression(" << (char)tn->get() << ')' << endl);
    if ( !(condition = pgm.parseExpression(tn, true)) )
	pgm.Throw(tn) << "Failed to parse expression" << flush;

    tn = pgm.nextToken();  // consume ; separator between condition and increment
    if ( tn->id() != TokenID::tkSemi )
	pgm.Throw(tn) << "Expecting ';' after for condition" << flush;
    tn = pgm.nextToken();  // first token of increment expression
    DBG(cout << "TokenFOR::parse() increment: calling parseExpression(" << (char)tn->get() << ')' << endl);
    if ( !(increment = pgm.parseExpression(tn, true)) )
	pgm.Throw(tn) << "Failed to parse increment" << flush;

    // C comma-expression increment: `for (...; ...; i++, j--, k++)`.
    while ( pgm.peekToken() )
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

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkClBrk )
	pgm.Throw(tn) << "Expecting )" << flush;

    tn = pgm.nextToken();

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
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenWHILE::parse() expecting (" << endl);
	pgm.Throw(tn) << "expecting ( after while" << flush;
    }
    DBG(cout << "TokenWHILE::parse() calling parseExpression(" << (char)tn->get() << ')' << endl);
    condition = pgm.parseExpression(tn, true);

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
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenDO::parse() expecting (" << endl);
	pgm.Throw(tn) << "Expecting ( after while" << flush;
    }
    DBG(cout << "TokenDO::parse() calling parseExpression(" << (char)tn->get() << ')' << endl);
    condition = pgm.parseExpression(tn, true);

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
		Throw(tag) << "Unknown struct type '" << sname << "'" << flush;
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

	// Optional parameter name (discard)
	if ( peekToken() && peekToken()->type() == TokenType::ttIdentifier )
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
	if ( tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting identifier in enum" << flush;
	std::string name = ((TokenIdent *)pgm.nextToken())->str;

	// check for = explicit value
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkAssign )
	{
	    pgm.nextToken(); // consume '='
	    TokenBase *vt = pgm.nextToken();
	    if ( vt->type() == TokenType::ttInteger )
		val = vt->get();
	    else if ( vt->id() == TokenID::tkNeg )
	    {
		vt = pgm.nextToken();
		if ( vt->type() == TokenType::ttInteger )
		    val = -vt->get();
	    }
	    else
		pgm.Throw(vt) << "Expecting integer value in enum" << flush;
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

// extern — consume and pass through (declarations are just skipped)
TokenBase *TokenEXTERN::parse(Program &pgm)
{
    DBG(std::cout << "TokenEXTERN::parse() — consuming extern" << std::endl);
    // skip to semicolon
    TokenBase *tn;
    while ( (tn = pgm.nextToken()) && tn->id() != TokenID::tkSemi )
	;
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
	    // parse case value — must be a literal constant (integer, char, string)
	    tc->value = pgm.nextToken();
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
	func->parameters.push_back(&ddINT64); // void* as int64
	ids.push_back("__retbuf");
	DBG(cout << "parseFunction() injected hidden __retbuf for multi-return (" << multi_ret->size() << " types)" << endl);
    }

    // for class methods, inject hidden __this parameter as first arg
    if ( owner_class )
    {
	func->parameters.push_back(&ddINT64); // void* as int64
	ids.push_back("__this");
	DBG(cout << "parseFunction() injected hidden __this parameter for class method" << endl);
    }

    // look for parameters
    while ( (nt=nextToken()) && nt->id() != TokenID::tkClBrk )
    {
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
	    // inject hidden __va_args parameter (pointer to packed args buffer)
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
		Throw(tag_nt) << "Unknown struct type '" << sname << "'" << flush;
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
	if ( nt->type() != TokenType::ttIdentifier )
	{
	    Throw(nt) << "Expecting identifier after type" << flush;
	}

	// grab identifier string
	pid = ((TokenIdent *)nt)->str;
	if ( !peekToken() )
	    Throw(nt) << "Expecting token after identifier" << flush;

	nt = nextToken();

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
	// should make sure type is correct
	method = (Method *)var->data;
    }
    else
    {
	var = addVariable(NULL, *func, id);
	method = new Method(*var);
	var->data = (void *)method;
    }

    if ( owner_class )
	method->owner_class = owner_class;

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
	Throw(nt) << "Expecting brace after function declaration" << flush;
    }

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

	if ( tn->type() != TokenType::ttIdentifier )
	    Throw(tn) << "Expecting identifier in lambda parameter list" << flush;

	std::string pid = ((TokenIdent *)tn)->str;
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

    DBG(std::cout << "parseDeclaration(" << tb->str << ") START " << (tb->file ? tb->file : "NULL") << ':' << tb->line << ':' << tb->column << std::endl);

    // check for pointer declarator(s): type * [*...] identifier.
    // base_type is the declared type without any `*`s — comma-continuations
    // later in this function start fresh from base_type because each var
    // in `char *p, *q;` has its own `*`s, not cumulative.
    DataDef *base_type = &tb->definition;
    DataDef *decl_type = base_type;
    // Function-pointer typedefs (DataDefFPTR) already represent pointers;
    // `DO_FUN *cmd;` and `DO_FUN cmd;` both name a function-pointer variable.
    bool is_fnptr_base = (dynamic_cast<DataDefFPTR *>(base_type) != NULL);
    while ( peekToken() && peekToken()->id() == TokenID::tkMul )
    {
	nextToken(); // consume '*'
	if ( !is_fnptr_base )
	    decl_type = getPointerType(decl_type);
	DBG(std::cout << "parseDeclaration() pointer: " << decl_type->name << std::endl);
    }

    if ( !peekToken() )
	Throw(tb) << "Unexpected end of data: Expecting identifier after type" << flush;
    nt = nextToken();

    if ( nt->type() != TokenType::ttIdentifier )
    {
	DBG(cerr << "parseDeclaration() nt->type()=" << (int)nt->type() << endl);
	Throw(nt) << "Expecting identifier after type" << flush;
    }
    // grab identifier string
    id = ((TokenIdent *)nt)->str;
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
	    TokenBase *sz = nextToken();
	    if ( !sz || sz->type() != TokenType::ttInteger )
		Throw(sz ? sz : tb) << "Fixed-size array dimension must be an integer constant" << flush;
	    int64_t n = ((TokenInt *)sz)->get();
	    if ( n <= 0 )
		Throw(sz) << "Fixed-size array dimension must be positive" << flush;
	    arr_dims.push_back((uint32_t)n);
	    TokenBase *cl = nextToken();
	    if ( !cl || cl->id() != TokenID::tkClSqr )
		Throw(cl ? cl : tb) << "Expected ] in array declaration" << flush;
	}
	nt = peekToken();
	if ( !nt )
	    Throw(tb) << "Unexpected end of data in array declaration" << flush;
    }

    // Sugar: char *name = "literal"  →  treat as  char name[]  with string init.
    // This matches C's practical equivalence where a string-literal-initialized
    // char pointer and a char array are interchangeable at the point of use.
    if ( nt->id() == TokenID::tkAssign && arr_dims.empty() )
    {
	DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(decl_type);
	if ( pdd && pdd->base_type == &ddCHAR )
	{
	    TokenBase *eq_tok = nextToken(); // consume '=' to peek past it
	    TokenBase *str_peek = peekToken();
	    if ( str_peek && str_peek->type() == TokenType::ttString )
	    {
		decl_type = &ddCHAR;
		arr_dims.push_back(0); // size inferred from the string literal
	    }
	    pushToken(eq_tok); // put '=' back for the existing init path
	    nt = peekToken();
	}
    }

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
		TokenBase *strtok = nextToken(); // consume the string literal
		const std::string &s = ((TokenStr *)strtok)->str;
		for ( char c : s )
		    init_list.push_back(new TokenInt((int64_t)(unsigned char)c));
		init_list.push_back(new TokenInt(0)); // null terminator
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
		    // array-of-structs, or a nested struct member value).
		    nextToken(); // consume inner '{'
		    TokenStructLit *slit = new TokenStructLit();
		    while ( true )
		    {
			TokenBase *iln = peekToken();
			if ( !iln )
			    Throw(tb) << "Unexpected end of data in nested initializer" << flush;
			if ( iln->id() == TokenID::tkClBrc )
			{
			    nextToken(); // consume inner '}'
			    break;
			}
			TokenBase *iexpr = parseExpression(nextToken());
			slit->inits.push_back(iexpr);
			TokenBase *isep = peekToken();
			if ( isep && isep->id() == TokenID::tkComma )
			    nextToken();
		    }
		    init_list.push_back(slit);
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
		    Throw(nt) << "Array size missing and no initializer" << flush;
	}

	bool alloc = (!code || gotstatic) ? true : false;
	uint32_t elem_count = 1;
	for ( auto d : arr_dims ) elem_count *= d;
	var = addVariable(code, *decl_type, id, elem_count, NULL, alloc);
	if ( gotstatic )
	    var->flags |= vfSTATIC;
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

	if ( nt->id() == TokenID::tkAssign && arr_dims.empty() )
	{
	    DBG(std::cout << "parseDeclaration() calling td->initialize = parseExpression" << std::endl);
	    // conditional=true so `;` stops without being consumed, which lets
	    // the comma-continuation loop below distinguish "end of decl" (peek
	    // is `;`) from "more decls" (peek is `,` or the next identifier).
	    td->initialize = parseExpression(new TokenVar(*var), true);
	}

	// Comma-continuation: `int a, b = 1, c;` — after the first decl, if the
	// next token is `,` (or parseExpression already consumed one and left
	// an identifier/`*` next), inject a clone of the base type token back
	// into the stream. parseCompound's next iteration sees it as a fresh
	// `int ...` statement and calls parseDeclaration recursively, which
	// handles pointer decorators, initializers, and further commas.
	if ( arr_dims.empty() )
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
