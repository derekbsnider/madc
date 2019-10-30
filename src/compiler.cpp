//////////////////////////////////////////////////////////////////////////
//									//
// madc "compiler" methods to compile the AST into x86 code		//
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

// construct a string at ptr address
void *string_construct(void *ptr)
{
    DBG(cout << "string_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::string;
}

// construct a stringstream at ptr address
void *stringstream_construct(void *ptr)
{
    DBG(cout << "stringstream_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::stringstream;
}

// construct an ostream at ptr address, with init
void *ostream_construct(void *ptr, void *init)
{
    DBG(cout << "ostream_construct(" << (uint64_t)ptr << ", " << (uint64_t)init << ')' << endl);
    return new(ptr) std::ostream((streambuf *)init);
}

// destruct a string at ptr address
void string_destruct(void *ptr)
{
    DBG(cout << "string_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::string *)ptr)->~string();
}

// destruct a stringstream at ptr address
void stringstream_destruct(void *ptr)
{
    DBG(cout << "stringstream_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::stringstream *)ptr)->~stringstream();
}

// destruct an ostream at ptr address
void ostream_destruct(void *ptr)
{
    DBG(cout << "ostream_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::ostream *)ptr)->~ostream();
}

// call string assign method, TODO: call directly
void string_assign(std::string &o, std::string &n)
{
    DBG(cout << "string_assign(" << o << '['<< (uint64_t)&o << "], " << n << '[' << (uint64_t)&n << "])" << endl);
    o.assign(n);
    DBG(cout << "string_assign(" << o << '['<< (uint64_t)&o << "])" << endl);
    DBG(cout << "string_assign(" << o << "::c_str()["<< (uint64_t)o.c_str() << "])" << endl);
}

void streamout_string(std::ostream &os, std::string &s)
{
//  DBG(std::cout << "streamout_string: << " << (uint64_t)&s << std::endl);
    os << s;
}

void streamout_cstr(std::ostream &os, const char *s)
{
//  DBG(std::cout << "streamout_string: << " << (uint64_t)&s << std::endl);
    os << s;
}

void streamout_int(std::ostream &os, int i)
{
//  DBG(std::cout << "streamout_int: << " << i << std::endl);
    os << i;
}

template<typename T> void streamout_numeric(std::ostream &os, T i)
{
//  DBG(std::cout << "streamout_numeric: sizeof(i) " << sizeof(i) << std::endl);
    os << i;
}

void streamout_intptr(std::ostream &os, int *i)
{
    if ( !i ) { std::cerr << "ERROR: streamout_intptr: NULL!" << std::endl; return; }
    DBG(std::cout << "streamout_intptr: << " << *i << std::endl);
    os << *i;
}


void Program::_compiler_init()
{
    DBG(static FileLogger logger(stdout));

    DBG(logger.setFlags(FormatOptions::kFlagDebugRA | FormatOptions::kFlagMachineCode | FormatOptions::kFlagDebugPasses));

    code.reset();
    code.init(jit.codeInfo());
    DBG(code.setLogger(&logger));
//  this seems to break things at times
//  code.addEmitterOptions(BaseEmitter::kOptionStrictValidation);
    code.attach(&cc);
    // constant initialization
//  __const_double_1 = cc.newDoubleConst(ConstPool::kScopeGlobal, 1.0);
}

bool Program::_compiler_finalize()
{
    cc.ret(); // extra ret just in case
    if ( !cc.finalize() )
    {
	std::cerr << "Finalize failed!" << std::endl;
	return false;
    }
    asmjit::Error err = jit.add(&root_fn, &code);
    if ( !root_fn )
    {
	std::cerr << "Code generation failed!" << std::endl;
	switch(err)
	{
	    case kErrorNoCodeGenerated: std::cerr << "No code generated" << std::endl; break;
	    case kErrorInvalidSection: std::cerr << "Invalid section" << std::endl; break;
	    case kErrorTooManySections: std::cerr << "Too many sections" << std::endl; break;
	    case kErrorInvalidSectionName: std::cerr << "Invalid section name" << std::endl; break;
	    case kErrorTooManyRelocations: std::cerr << "Too many relocations" << std::endl; break;
	    case kErrorInvalidRelocEntry: std::cerr << "Invalid relocation entry" << std::endl; break;
	    case kErrorRelocOffsetOutOfRange: std::cerr << "Reloc entry contains address that is out of range (unencodable)" << std::endl; break;
	    case kErrorInvalidAssignment: std::cerr << "Invalid assignment to a register, function argument, or function return value" << std::endl; break;
	    case kErrorInvalidInstruction: std::cerr << "Invalid instruction" << std::endl; break;
	    case kErrorInvalidRegType: std::cerr << "Invalid register type" << std::endl; break;
	    default: std::cerr << "Error number " << err << std::endl; break;
	}
	return false;
    }
    variable_vec_iter vvi;
    Variable *var;
    Method *method;
    FuncNode *fnd;

    // find all global variables which are functions, have no x86code assigned
    // and have a funcnode label, so that we can properly set our function pointer
    for ( vvi = tkProgram->variables.begin(); vvi != tkProgram->variables.end(); ++vvi )
    {
	var = *vvi;
	if ( var->type->basetype() == BaseType::btFunct
	&&   (method=(Method *)var->data) && !method->x86code
	&&   (fnd=((FuncDef *)(method->returns.type))->funcnode) )
	    method->x86code = (uint8_t *)root_fn + code.labelOffset(fnd->label());
    }

    return true;
}


Operand &TokenCallMethod::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCallMethod::compile(" << var.name << ") TOP" << endl);
    DBG(pgm.cc.comment("TokenCallMethod start for "));
    DBG(pgm.cc.comment(object.name.c_str()));
    DBG(pgm.cc.comment("::"));
    DBG(pgm.cc.comment(var.name.c_str()));
    regdp.object = &pgm.tkFunction->voperand(pgm, &object);
    return TokenCallFunc::compile(pgm, regdp);
}

Operand &TokenCallFunc::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCallFunc::compile(" << var.name << ") TOP" << endl);
    DBG(pgm.cc.comment("TokenCallFunc start for "));
    DBG(pgm.cc.comment(var.name.c_str()));

    if ( !var.type->is_function() )
	pgm.Throw(this) << "TokenCallFunc::compile() called on non-function" << flush;

    Method *method;
    FuncNode *fnd;

    // grab the Method object
    if ( !(method=(Method *)var.data) )
	pgm.Throw(this) << "TokenCallFunc::compile() function method is NULL" << flush;

    // grab the FuncNode object
    if ( !(fnd=((FuncDef *)(method->returns.type))->funcnode) && !method->x86code )
	pgm.Throw(this) << "TokenCallFunc::compile() method has neither FuncNode nor x86code" << flush;

    // build arguments
    FuncDef *func = (FuncDef *)method->returns.type;
    FuncSignatureBuilder funcsig(CallConv::kIdHost);
    std::vector<Operand> params;
    DataDef *ptype;
    uint32_t _argc;
    TokenBase *tn;

    if ( !regdp.second )
    {
	DBG(cout << "TokenCallFunc::compile(" << var.name << ") regdp.second = " << func->returns.name << endl);
	regdp.second = &func->returns;
    }

    DBG(cout << "TokenCallFunc::compile(" << var.name << ") func->returns.type() " << (int)func->returns.type() << endl);

    // set return type
    switch(func->returns.type())
    {
	case DataType::dtVOID:		funcsig.setRetT<void>();		break;
	case DataType::dtCHAR:		funcsig.setRetT<char>();		break;
	case DataType::dtBOOL:		funcsig.setRetT<bool>();		break;
	case DataType::dtINT16:		funcsig.setRetT<int16_t>();		break;
	case DataType::dtINT24:		funcsig.setRetT<int16_t>();		break;
	case DataType::dtINT32:		funcsig.setRetT<int32_t>();		break;
/*	case DataType::dtINT:		(same as dtINT64)			*/
	case DataType::dtINT64:		funcsig.setRetT<int64_t>();		break;
	case DataType::dtUINT8:		funcsig.setRetT<uint8_t>();		break;
	case DataType::dtUINT16:	funcsig.setRetT<uint16_t>();		break;
	case DataType::dtUINT24:	funcsig.setRetT<uint16_t>();		break;
	case DataType::dtUINT32:	funcsig.setRetT<uint32_t>();		break;
	case DataType::dtUINT64:	funcsig.setRetT<uint64_t>();		break;
	case DataType::dtCHARptr:	funcsig.setRetT<const char *>();	break;
	case DataType::dtSTRING:	funcsig.setRetT<void *>();		break;
	default:			funcsig.setRetT<void *>();		break;
    }
#if 1
    if ( !regdp.first )
    {
	DBG(pgm.cc.comment("TokenCallFunc::compile() operand() to assign _operand"));
	regdp.first = &operand(pgm); // assign _reg if not provided
//	regdp.first = &_reg;
    }
#endif

//#if OBJECT_SUPPORT
    // pass along object ("this") as first argument if appropriate
    if ( regdp.object )
    {
	funcsig.addArgT<void *>();
	params.push_back(*regdp.object);
	DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(*regdp.object)"));
    }
//#endif

    if ( argc() > func->parameters.size() )
    {
	std::cerr << "ERROR: TokenCallFunc::compile() method " << var.name << " called with too many parameters" << std::endl;
	std::cerr << "argc(): " << argc() << " func->parameters.size(): " << func->parameters.size() << std::endl;
	for ( size_t i = 0; i < argc(); ++i )
	{
	    tn = parameters[i];
	    std::cerr << "arg[" << i << "] type() = " << (int)tn->type() << " id() = " << (int)tn->id() << std::endl;
	}
	pgm.Throw(this) << "TokenCallFunc::compile() called with too many parameters" << flush;
    }

    for ( size_t i = 0; i < argc(); ++i )
    {
	regdefp_t funcrdp;
	ptype = func->parameters[i];
	tn = parameters[i];

	DBG(pgm.cc.comment("TokenCallFunc::argc param"));

	funcrdp.object = NULL; // should this be regdp.object?
	funcrdp.second = ptype;// this may result in an unwanted movsx on an unsigned integer type
//	_operand = ptype->newreg(pgm.cc);
//	funcrdp.first = &_operand;
	funcrdp.first = NULL; // clean for param
	Operand &tnreg = tn->compile(pgm, funcrdp);
	if ( !funcrdp.second )
	    pgm.Throw(tn) << "Failed to detemine type of rval" << flush;
	if ( ptype->is_numeric() && !funcrdp.second->is_numeric() )
	{
	    DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)funcrdp.second->type() << endl);
	    pgm.Throw(tn) << "Expecting numeric argument" << flush;
	}
	if ( ptype->is_integer() && !funcrdp.second->is_integer() )
	{
	    DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)funcrdp.second->type() << endl);
	    pgm.Throw(tn) << "Expecting integer argument" << flush;
	}
	if ( ptype->is_real() && !funcrdp.second->is_real() )
	{
	    DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)funcrdp.second->type() << endl);
	    pgm.Throw(tn) << "Expecting floating point argument" << flush;
	}
	if ( ptype->is_string() && !funcrdp.second->is_string() )
	    pgm.Throw(tn) << "Expecting string argument" << flush;
	if ( ptype->is_object() )
	{
	    if ( !funcrdp.second->is_object() )
		pgm.Throw(tn) << "Expecting object argument" << flush;
	    // check for has_ostream / has_istream
	    if ( ptype->rawtype() != funcrdp.second->rawtype() )
		pgm.Throw(tn) << "Object type mismatch" << flush;
	}
	DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(tnreg)"));
	if ( tnreg.isReg() && tnreg.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	{
	    if ( !ptype->is_real() )
		pgm.Throw(tn) << "Not expecting floating point argument" << flush;
	    DBG(pgm.cc.comment("tnreg is Xmm"));
	}
	if ( tnreg.isReg() && tnreg.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	{
	    if ( ptype->is_real() )
		pgm.Throw(tn) << "Expecting floating point argument" << flush;
	    DBG(pgm.cc.comment("tnreg is Gp"));
            DBG(cout << "tnreg size=" << tnreg.size() << " regdp.second->size=" << funcrdp.second->size << " type " << funcrdp.second->name << endl);
	}
	if ( tnreg.isImm() )
	    pgm.cc.comment("tnreg is Imm");
	params.push_back(tnreg); // params.push_back(pgm.tkFunction->getreg(pgm.cc, &tv->var));
	// could probably use a tv->var.addArgT(funcsig) method
	DBG(pgm.cc.comment(ptype->name.c_str() /*funcrdp.second->name.c_str()*/));
	switch(funcrdp.second->type())
	{
	    case DataType::dtCHAR:	funcsig.addArgT<char>();	break;
	    case DataType::dtBOOL:	funcsig.addArgT<bool>();	break;
//	    case DataType::dtINT:	funcsig.addArgT<int>();		break;
//	    case DataType::dtINT8:	funcsig.addArgT<int8_t>();	break;
	    case DataType::dtINT16:	funcsig.addArgT<int16_t>();	break;
	    case DataType::dtINT24:	funcsig.addArgT<int16_t>();	break;
	    case DataType::dtINT32:	funcsig.addArgT<int32_t>();	break;
	    case DataType::dtINT64:	funcsig.addArgT<int64_t>();	break;
	    case DataType::dtUINT8:	funcsig.addArgT<uint8_t>();	break;
	    case DataType::dtUINT16:	funcsig.addArgT<uint16_t>();	break;
	    case DataType::dtUINT24:	funcsig.addArgT<uint16_t>();	break;
	    case DataType::dtUINT32:	funcsig.addArgT<uint32_t>();	break;
	    case DataType::dtUINT64:	funcsig.addArgT<uint64_t>();	break;
	    case DataType::dtFLOAT:	funcsig.addArgT<float>();	break;
	    case DataType::dtDOUBLE:	funcsig.addArgT<double>();	DBG(pgm.cc.comment("addArgT<double>()")); break;
	    case DataType::dtCHARptr:	funcsig.addArgT<const char *>();break;
	    default:			funcsig.addArgT<void *>();	break;
	} // switch
    }

    if ( !fnd )
	DBG(std::cout << "TokenCallFunc::compile(cc.call(" << (uint64_t)method->x86code << ')' << std::endl);

    // now we should have all we need to call the function
    DBG(pgm.cc.comment("pgm.call:"));
    DBG(pgm.cc.comment(var.name.c_str()));
    FuncCallNode *call = fnd ? pgm.cc.call(fnd->label(), funcsig) : pgm.cc.call(imm(method->x86code), funcsig);
    std::vector<Operand>::iterator gvi;
    _argc = 0;

    DBG(pgm.cc.comment("TokenCallFunc::compile() looping over params"));
    for ( gvi = params.begin(); gvi != params.end(); ++gvi )
    {
	DBG(std::cout << "TokenCallFunc::compile(call->setArg(" << _argc << ", reg)" << endl);
	DBG(pgm.cc.comment("TokenCallFunc::compile(call->setArg())"));
	
	if ( gvi->isReg() )
	{
	    if ( gvi->as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    {
		DBG(pgm.cc.comment("call->setArg(_argc++, gvi->as<x86::Xmm>())"));
		call->setArg(_argc++, gvi->as<x86::Xmm>());
	    }
	    else
	    if ( gvi->as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    {
		DBG(pgm.cc.comment("call->setArg(_argc++, gvi->as<x86::Gp>())"));
		call->setArg(_argc++, gvi->as<x86::Gp>());
	    }
	    else
		pgm.Throw(tn) << "TokenCallFunc::compile() unexpected parameter Operand" << flush;
	}
	else
	if ( gvi->isImm() )
	{
	    DBG(pgm.cc.comment("call->setArg(_argc++, gvi->as<Imm>())"));
	    call->setArg(_argc++, gvi->as<Imm>());
	}
	else
	if ( gvi->isMem() )
	{
	    x86::Gp tmp = pgm.cc.newGpq();
	    pgm.cc.mov(tmp, gvi->as<x86::Mem>());
	    call->setArg(_argc++, tmp);
	    DBG(pgm.cc.comment("call->setArg(_argc++, gvi->as<Mem>())"));
	}
    }

    DBG(std::cout << "TokenCallFunc::compile() END" << std::endl);

    if ( !regdp.second )
	regdp.second = &func->returns;

#if 1
    // handle return value
    if ( regdp.first )
    {
	if ( !regdp.first->isReg() )
	    call->_setRet(0, *regdp.first);
//	    throw "TokenCallFunc::compile() regdp.first->isReg() is FALSE";
	if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    call->setRet(0, regdp.first->as<x86::Xmm>());
	else
	if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	{
	    DBG(pgm.cc.comment("call->setRet(0, regdp.first->as<x86::Gp>())"));
	    call->setRet(0, regdp.first->as<x86::Gp>());
	}
	DBG(pgm.cc.comment("TokenCallFunc::compile() regdp.first END"));
	return *regdp.first;
    }
    else
#endif
    if ( func->returns.type() != DataType::dtVOID )
    { 
	if ( func->returns.is_real() )
	    call->setRet(0, _operand.as<x86::Xmm>());
	else
	    call->setRet(0, _operand.as<x86::Gp>());
	regdp.first = &_operand;
    }
    DBG(pgm.cc.comment("TokenCallFunc::compile() END"));

    return _operand;
}

Operand &TokenCpnd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") TOP" << endl);
    Operand *operand = NULL;
    for ( vector<TokenStmt *>::iterator vti = statements.begin(); vti != statements.end(); ++vti )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	operand = &(*vti)->compile(pgm, regdp);
    }
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") END" << endl);

    if ( !regdp.first )
	regdp.first = &_operand;
    return *regdp.first;
}

// compile the "program" token, which contains all initilization / non-function statements
Operand &TokenProgram::compile(Program &pgm, regdefp_t &regdp)
{
    if ( this != pgm.tkProgram ) { throw "this != tkProgram"; }
    DBG(cout << "TokenProgram::compile(" << (uint64_t)this << ") TOP" << endl);
    DBG(cout << "    source: " << source << endl);
    DBG(cout << "     bytes: " << bytes << endl);
    DBG(cout << "     lines: " << lines << endl);

    pgm.tkFunction = pgm.tkProgram;
    pgm.tkFunction->clear_operand_map(); // clear operand map

    pgm.cc.addFunc(FuncSignatureT<void, void>(CallConv::kIdHost));

    for ( vector<TokenStmt *>::iterator si = statements.begin(); si != statements.end(); ++si )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	(*si)->compile(pgm, regdp);
    }

    pgm.tkFunction->cleanup(pgm.cc);	// cleanup stack
    pgm.cc.ret();			// always add return in case source doesn't have one
    pgm.cc.endFunc();			// end function

    pgm.tkFunction->clear_operand_map(); // clear operand map

    DBG(cout << "TokenProgram::compile(" << (uint64_t)this << ") END" << endl);

    return _operand;
}

Operand &TokenBase::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenStmt::compile(" << (void *)this << " type: " << (int)type() << (regdp.first ? " ret=true" : "") << ") TOP" << endl);
    switch(type())
    {
	case TokenType::ttOperator:
	    DBG(cout << "TokenOperator::compile(" << (char)get() << ')' << endl);
	    return dynamic_cast<TokenOperator *>(this)->compile(pgm, regdp);
	case TokenType::ttMultiOp:
	    DBG(cout << "TokenMultiOp::compile()" << endl);
	    return dynamic_cast<TokenMultiOp *>(this)->compile(pgm, regdp);
	case TokenType::ttIdentifier:
	    DBG(cout << "TokenStmt::compile() TokenIdent(" << ((TokenIdent *)this)->str << ')' << endl);
	    break;
	case TokenType::ttKeyword:
	    return dynamic_cast<TokenKeyword *>(this)->compile(pgm, regdp);
	case TokenType::ttDataType:
	    DBG(cout << "TokenStmt::compile() TokenDataType(" << ((TokenDataType *)this)->definition.name << ')' << endl);
	    break;
	case TokenType::ttInteger:
	    DBG(cout << "TokenStmt::compile() TokenInt(" << ival() << ')' << endl);
	    return dynamic_cast<TokenInt *>(this)->compile(pgm, regdp);
	case TokenType::ttReal:
	    DBG(cout << "TokenStmt::compile() TokenReal(" << ((TokenReal *)this)->dval() << ')' << endl);
	    return dynamic_cast<TokenReal *>(this)->compile(pgm, regdp);
	case TokenType::ttVariable:
	    DBG(cout << "TokenStmt::compile() TokenVar(" << dynamic_cast<TokenVar *>(this)->var.name << ')' << endl);
	    return dynamic_cast<TokenVar *>(this)->compile(pgm, regdp);
	case TokenType::ttCallFunc:
	    return dynamic_cast<TokenCallFunc *>(this)->compile(pgm, regdp);
	case TokenType::ttCallMethod:
	    return dynamic_cast<TokenCallMethod *>(this)->compile(pgm, regdp);
	case TokenType::ttDeclare:
	    return dynamic_cast<TokenDecl *>(this)->compile(pgm, regdp);
	case TokenType::ttFunction:
	    return dynamic_cast<TokenFunc *>(this)->compile(pgm, regdp);
	case TokenType::ttStatement:
	    // ttStatement should not be used anywhere
	    throw "TokenStmt::compile() tb->type() == TokenType::ttStatement";
	case TokenType::ttCompound:
	    return dynamic_cast<TokenCpnd *>(this)->compile(pgm, regdp);
	case TokenType::ttProgram:
	    return dynamic_cast<TokenProgram *>(this)->compile(pgm, regdp);
	case TokenType::ttSymbol:
	    if ( id() == TokenID::tkSemi )
	    {
		DBG(cout << "TokenStmt::compile() TokenSymbol(;) NOOP" << endl);
		break;
	    }
	default:
	    DBG(cerr << "TokenStmt::compile() throwing unexpected token" << endl);
	    throw this;
    } // end switch
    DBG(cout << "TokenStmt::compile(" << (void *)this << ") END" << endl);
    return _reg;
}

Operand &TokenDecl::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDecl::compile(" << var.name << " regdp.second: " << (regdp.second ? regdp.second->name : "")<<  ") TOP" << endl);

    if ( initialize )
	initialize->compile(pgm, regdp);

    DBG(cout << "TokenDecl::compile(" << var.name << ") END" << endl);

    return _reg;
}

Operand &TokenFunc::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenFunc::compile(" << var.name << '[' << (uint64_t)this << "]) TOP" << endl);
    if ( !var.data ) { throw "TokenFunc::compile: method is NULL"; }

    Method &method = *((Method *)var.data);
    FuncDef *func = (FuncDef *)method.returns.type;
    FuncSignatureBuilder funcsig(CallConv::kIdHost);
    datadef_vec_iter dvi;

    // set return type
    /**/ if ( &func->returns == &ddSTRING ) { funcsig.setRetT<const char *>(); }
    else if ( &func->returns == &ddCHAR   ) { funcsig.setRetT<char>();         }
    else if ( &func->returns == &ddBOOL   ) { funcsig.setRetT<bool>();         }
    else if ( &func->returns == &ddINT    ) { funcsig.setRetT<int>();          }
    else if ( &func->returns == &ddINT8   ) { funcsig.setRetT<int8_t>();       }
    else if ( &func->returns == &ddINT16  ) { funcsig.setRetT<int16_t>();      }
    else if ( &func->returns == &ddINT24  ) { funcsig.setRetT<int16_t>();      }
    else if ( &func->returns == &ddINT32  ) { funcsig.setRetT<int32_t>();      }
    else if ( &func->returns == &ddINT64  ) { funcsig.setRetT<int64_t>();      }
    else if ( &func->returns == &ddUINT8  ) { funcsig.setRetT<uint8_t>();      }
    else if ( &func->returns == &ddUINT16 ) { funcsig.setRetT<uint16_t>();     }
    else if ( &func->returns == &ddUINT24 ) { funcsig.setRetT<uint16_t>();     }
    else if ( &func->returns == &ddUINT32 ) { funcsig.setRetT<uint32_t>();     }
    else if ( &func->returns == &ddUINT64 ) { funcsig.setRetT<uint64_t>();     }
    else       /* default condition */      { funcsig.setRetT<void *>();       }

    // set parameter types
    for ( dvi = func->parameters.begin(); dvi != func->parameters.end(); ++dvi )
    {
	/**/ if ( *dvi == &ddSTRING ) { funcsig.addArgT<const char *>(); }
	else if ( *dvi == &ddCHAR   ) { funcsig.addArgT<char>();         }
	else if ( *dvi == &ddBOOL   ) { funcsig.addArgT<bool>();         }
	else if ( *dvi == &ddINT    ) { funcsig.addArgT<int>();          }
	else if ( *dvi == &ddINT8   ) { funcsig.addArgT<int8_t>();       }
	else if ( *dvi == &ddINT16  ) { funcsig.addArgT<int16_t>();      }
	else if ( *dvi == &ddINT24  ) { funcsig.addArgT<int16_t>();      }
	else if ( *dvi == &ddINT32  ) { funcsig.addArgT<int32_t>();      }
	else if ( *dvi == &ddINT64  ) { funcsig.addArgT<int64_t>();      }
	else if ( *dvi == &ddUINT8  ) { funcsig.addArgT<uint8_t>();      }
	else if ( *dvi == &ddUINT16 ) { funcsig.addArgT<uint16_t>();     }
	else if ( *dvi == &ddUINT24 ) { funcsig.addArgT<uint16_t>();     }
	else if ( *dvi == &ddUINT32 ) { funcsig.addArgT<uint32_t>();     }
	else if ( *dvi == &ddUINT64 ) { funcsig.addArgT<uint64_t>();     }
	else /* default condition */  { funcsig.addArgT<void *>();       }
    }

    if ( !(func->funcnode=pgm.cc.newFunc(funcsig)) )
    {
	std::cerr << "Failed to create funcnode!" << std::endl;
	throw "Failed to create funcnode";
    }

    pgm.tkFunction = this;
    clear_operand_map(); // clear operand map

    pgm.cc.addFunc(func->funcnode);

    if ( method.parameters.size() )
    {
	DBG(cout << "TokenFunc::compile() has parameters:" << endl);
	uint32_t argc = 0;

	for ( variable_vec_iter vvi = method.parameters.begin(); vvi != method.parameters.end(); ++vvi )
	{
	    DBG(std::cout << "TokenFunc::compile(): cc.setArg(" << argc << ", " << (*vvi)->name << ')' << std::endl);
	    Operand &reg = voperand(pgm, (*vvi));

	    if ( reg.isReg() )
	    {
		if ( reg.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
		    pgm.cc.setArg(argc++, reg.as<x86::Xmm>());
		else
		if ( reg.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
		    pgm.cc.setArg(argc++, reg.as<x86::Gp>());
		else
		    throw "TokenFunc::compile() unexpected parameter Operand";
	    }
	    else
		throw "TokenFunc::compile() argument not register";

	    (*vvi)->flags |= vfREGSET;
	}
    }

    if ( variables.size() )
    {
	DBG(cout << "Local variables:" << endl);
	for ( variable_vec_iter vvi = variables.begin(); vvi != variables.end(); ++vvi )
	{
	    DBG(cout << "    " << (*vvi)->type->name << ' ' << (*vvi)->name << endl);
	}
    }

    for ( vector<TokenStmt *>::iterator si = statements.begin(); si != statements.end(); ++si )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	(*si)->compile(pgm, regdp);
    }

    cleanup(pgm.cc);	// cleanup stack
    pgm.cc.ret();	// always add return in case source doesn't have one
    pgm.cc.endFunc();	// end function

    clear_operand_map();// clear operand map

    DBG(cout << "TokenFunc::compile(" << var.name << ") END" << endl);

    return _reg;
}


// compile the code tree into x86 code
bool Program::compile()
{
    TokenBase *tb;
    regdefp_t regdp = {NULL, NULL, NULL};

    DBG(cout << endl << endl << "Program::compile() start" << endl << endl);
    _compiler_init();

    try
    {
	while ( !ast.empty() )
	{
	    tb = ast.front();
	    DBG(cout << "Program::compile(" << (void *)tb << ')' << endl);
	    ast.pop();
	    // each new statement starts with a clean slate
	    regdp = {NULL, NULL, NULL};
	    tb->compile(*this, regdp);
	}
    }
    catch(const char *err_msg)
    {
	if ( tb )
	    cerr << ANSI_WHITE << (tb->file ? tb->file : "NULL") << ':' << tb->line << ':' << tb->column;
	else
	    cerr << ANSI_WHITE;
	cerr << ": \e[1;31merror:\e[1;37m " << err_msg << ANSI_RESET << endl;
	if ( tb )
	{
	    source.showerror(tb->line, tb->column);
	    cout << "TokenType: " << (int)tb->type() << endl;
	}
	return false;
    }
    catch(TokenBase *tb)
    {
	cerr << ANSI_WHITE << (tb->file ? tb->file : "NULL") << ':' << tb->line << ':' << tb->column
	     << ": \e[1;31merror:\e[1;37m unexpected token type " << (int)tb->type() << " value " << (int)tb->get() << " char " << (char)tb->get() << ANSI_RESET << endl;
	source.showerror(tb->line, tb->column);
	return false;
    }
    catch(std::exception &e)
    {
	return false;
    }

    DBG(cout << "Program::compile() done" << endl);

    return _compiler_finalize();
}

// execute main function
void Program::execute()
{
    std::string main("main");
    Variable *var = findVariable(main);
    Method *method;
    fVOIDFUNC main_fn;

    DBG(std::cout << "Program::execute() calling root_fn()" << std::endl);
    root_fn();

    if ( !var )
    {
	DBG(std::cerr << "Program::execute() cannot find main" << std::endl);
	return;
    }
    if ( var->type->basetype() != BaseType::btFunct )
    {
	std::cerr << "Program::execute() main is not a function" << std::endl;
	return;
    }
    if ( !(method=(Method *)var->data) )
    {
	std::cerr << "Program::execute() main method is NULL" << std::endl;
	return;
    }
    if ( !(main_fn=(fVOIDFUNC)method->x86code) )
    {
	std::cerr << "Program::execute() main has no x86 code" << std::endl;
	return;
    }
    DBG(std::cout << std::endl << "Program::execute() starts" << std::endl);    
    DBG(std::cout << "Program::execute() calling main()[" << std::hex << ((uint64_t)main_fn) << std::dec << ']' << std::endl << std::endl);
    main_fn();
    DBG(std::cout << std::endl << "Program::execute() main() returns" << std::endl);
    DBG(std::cout << "Program::execute() ends" << std::endl);    
}

// compile the increment operator
Operand &TokenInc::compile(Program &pgm, regdefp_t &regdp)
{
    TokenVar *tv;

    // left has precedence over right
    if ( left )
    {
	if ( left->type() != TokenType::ttVariable )
	    throw "Increment on a non-variable lval";
	tv = dynamic_cast<TokenVar *>(left);
	Operand &reg = tv->operand(pgm);
	pgm.safeinc(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    if ( right )
    {
	if ( right->type() != TokenType::ttVariable )
	    throw "Increment on a non-variable rval";
	tv = dynamic_cast<TokenVar *>(right);
	Operand &reg = tv->operand(pgm);
	pgm.safeinc(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    throw "Invalid increment";
}

// compile the decrement operator
Operand &TokenDec::compile(Program &pgm, regdefp_t &regdp)
{
    TokenVar *tv;

    // left has precedence over right
    if ( left )
    {
	if ( left->type() != TokenType::ttVariable )
	    throw "Decrement on a non-variable lval";
	tv = dynamic_cast<TokenVar *>(left);
	Operand &reg = tv->operand(pgm);
	pgm.safedec(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    if ( right )
    {
	if ( right->type() != TokenType::ttVariable )
	    throw "Decrement on a non-variable rval";
	tv = dynamic_cast<TokenVar *>(right);
	Operand &reg = tv->operand(pgm);
	pgm.safedec(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    throw "Invalid increment";
}

// Basic assignment left = right
//
// Needs to respect regdp.first containing an operand from a previous left
// assignment so that x = y = 1 would pass along x's register operand to
// this TokenAssign, so that we can give it the same value along the chain
//
// Needs to understand that different variable types work different ways:
// - integers use newGpX of appropriate size, and get loaded with the value
// - real numbers use newXmm, and get loaded with a floating value
// - strings use a newIntPtr, and get loaded with an address
//
// If a variable is numeric, and local (on the stack) then we can make it
// super fast by using a register as much as possible, otherwise we have
// to load/save/update the memory so that possible external access gets
// the right data. Anytime we give up control to anything external to the
// local function, we will need to save the register to memory, and read it
// back again afterwards (unless it's being assigned to the return value)
//
// regdp is passed along to all TokenXXXX::compile() methods to share a
// register for the entire expression chain, as we want to avoid memory
// access until we need it
//
// reads from the stack are cached in a register, reads from global memory
// cannot be cached, as the memory may have changed outside of the function
// if the address of a stack variable is passed outside of the function
// by using a reference, then that variable is flagged and treated as a
// global variable for the rest of the life of the function, as it becomes
// possible that external operations could modify this memory
Operand &TokenAssign::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAssign::compile(" << (regdp.second ? regdp.second->name : "") << ") TOP" << endl);
    TokenVar *tvl = NULL;
    TokenMember *tml;
    DataDef *ltype = NULL;
    Operand *r_operand;

    if ( !left )  // = 1;
	throw "Assignment with no lval";
    if ( !right ) // x = ;
	throw "Assignment with no rval";

    DBG(pgm.cc.comment("TokenAssign start"));

    // handle variable token
    if ( left->type() == TokenType::ttVariable )
    {
	tvl = dynamic_cast<TokenVar *>(left);
	ltype = tvl->var.type;
	DBG(cout << "TokenAssign::compile() assignment to " << tvl->var.name << " type " << ltype->name << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() assignment to:"));
	DBG(pgm.cc.comment(tvl->var.name.c_str()));
	DBG(pgm.cc.comment(ltype->name.c_str()));
	DBG(pgm.cc.comment("TokenAssign::compile() _operand = tvl->operand(pgm)"));
	_operand = tvl->operand(pgm);
	if ( regdp.second && regdp.second != ltype
	&&  !regdp.second->is_compatible(*ltype) )
	{
	    cerr << "regdp.second->type() " << (int)regdp.second->type() << " name: " << regdp.second->name << endl;
	    cerr << "     tvl->var.type() " << (int)ltype->type() << " name: " << ltype->name << endl;
	    throw "incompatible assignment";
	}
	if ( regdp.second )
	    DBG(cout << "regdp.second->type() " << (int)regdp.second->type() << " name: " << regdp.second->name << endl);
	else
	    DBG(cout << "regdp.second is NULL" << endl);
	DBG(cout << "     tvl->var.type() " << (int)ltype->type() << " name: " << ltype->name << endl);
    }
    else
    // TODO: handle member token
    if ( left->type() == TokenType::ttMember )
    {
	tml = dynamic_cast<TokenMember *>(left);
	ltype = tml->var.type;
	DBG(cout << "TokenAssign::compile() assignment to " << tml->var.name << " type " << ltype->name << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() assignment to:"));
	DBG(pgm.cc.comment(tml->var.name.c_str()));
	DBG(pgm.cc.comment(ltype->name.c_str()));
	DBG(pgm.cc.comment("TokenAssign::compile() _operand = tml->operand(pgm)"));
	_operand = tml->operand(pgm);
	if ( regdp.second && regdp.second != ltype
	&&  !regdp.second->is_compatible(*ltype) )
	{
	    cerr << "regdp.second->type() " << (int)regdp.second->type() << " name: " << regdp.second->name << endl;
	    cerr << "     tml->var.type() " << (int)ltype->type() << " name: " << ltype->name << endl;
	    throw "incompatible assignment";
	}
	if ( regdp.second )
	    DBG(cout << "regdp.second->type() " << (int)regdp.second->type() << " name: " << regdp.second->name << endl);
	else
	    DBG(cout << "regdp.second is NULL" << endl);
	DBG(cout << "     tml->var.type() " << (int)ltype->type() << " name: " << ltype->name << endl);
    }
    else
    {
	pgm.Throw(this) << "Assignment on a non-variable lval" << flush;
    }

    if ( !regdp.first || !regdp.second )
	regdp.second = ltype; // set type if not set

    // we should have _operand set to our left variable at this point
    // only if our left variable is numeric do we want to pass it to
    // our right side, otherwise we want to clear it
    if ( ltype->is_numeric() )
    {
	Operand *orig_operand = regdp.first;
	regdp.first = &_operand;
	r_operand = &right->compile(pgm, regdp);
	if ( orig_operand )
	    regdp.first = orig_operand;
    }
    else
    // otherwise we need to have two operands and perform the
    // assignment using an assignment method on the object, or
    // by writing to the proper member of the class or structure
    {
	Operand *orig_operand = regdp.first;
	regdp.first = NULL;
	r_operand = &right->compile(pgm, regdp);
	regdp.first = orig_operand ? orig_operand : r_operand;
    }

    if ( !regdp.second )
	throw "TokenAssign: no rval type";
    if (  ltype->is_numeric() && !regdp.second->is_numeric() )
	throw "Expecting rval to be numeric";
    if ( !ltype->is_integer() &&  regdp.second->is_integer() )
	throw "Not expecting rval to be numeric";
    if ( ltype->is_string() && !regdp.second->is_string() )
	throw "Expecting rval to be string";

    if ( ltype->is_numeric() )
    {
	DBG(cout << "TokenAssign::compile() numeric to numeric" << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() numeric to numeric"));
	if ( tvl )
	{
	    tvl->var.modified();
	    tvl->putreg(pgm);
	}
    }
    else
    if ( ltype->is_string() )
    {
	DBG(cout << "TokenAssign::compile() string to string" << endl);
/*
	DBG(cout << "TokenAssign::compile() will call " << tvl->var.name << '('
	    << (tvl->var.data ? ((string *)(tvl->var.data))->c_str() : "") << ").assign[" << (uint64_t)string_assign << "](" << tvr->var.name
	    << '(' << (tvr->var.data ? ((string *)(tvr->var.data))->c_str() : "") << ')' << endl);
*/
	DBG(pgm.cc.comment("string_assign"));
	FuncCallNode* call = pgm.cc.call(imm(string_assign), FuncSignatureT<void, const char*, const char *>(CallConv::kIdHost));
	call->setArg(0, _operand.as<x86::Gp>());
	call->setArg(1, r_operand->as<x86::Gp>());
	if ( tvl )
	{
	    tvl->var.modified();
	    tvl->putreg(pgm);
	}
    }
    else
	throw "Unsupported assignment";

    DBG(cout << "TokenAssign::compile() END" << endl);

    return *regdp.first;
}


// operator gets 64bit set to 0
x86::Gp &TokenOperator::getreg(Program &pgm)
{
    _reg = pgm.cc.newGpq();
    pgm.cc.xor_(_reg, _reg);
    return _reg;
}

Operand &TokenOperator::operand(Program &pgm)
{
    if ( _datatype && _datatype->is_real() )
    {
	_operand = pgm.cc.newXmm();
	pgm.cc.xorps(_operand.as<x86::Xmm>(), _operand.as<x86::Xmm>());
    }
    else
    {
	_operand = pgm.cc.newGpq();
	pgm.cc.xor_(_operand.as<x86::Gp>(), _operand.as<x86::Gp>());
    }
    return _operand;
}


// returns the default operand for a token
// can be one of: x86::Gp, x86::Xmm or Imm
Operand &TokenBase::operand(Program &pgm)
{
    // default TokenBase just returns the immediate value of the token
    _operand = imm(_token);
    return _operand;
}


Operand &TokenInt::operand(Program &pgm)
{
    _operand = imm(_token);
    return _operand;
}

Operand &TokenReal::operand(Program &pgm)
{
    _const = pgm.cc.newDoubleConst(ConstPool::kScopeLocal, _val);
    _operand = pgm.cc.newXmm();
    DBG(pgm.cc.comment("TokenReal::operand() calling movsd(_operand.as<x86::Xmm>(), _const)"));
    DBG(cout << "TokenReal::operand() calling movsd(_operand.as<x86::Xmm>(), _const[" << _val << "])" << endl);
    pgm.cc.movsd(_operand.as<x86::Xmm>(), _const); //x86::qword_ptr((uintptr_t)&d_testval)); // x86::qword_ptr((uintptr_t)&_val));
    return _operand;
}

// variable needs special handling
Operand &TokenVar::operand(Program &pgm)
{
    return pgm.tkFunction->voperand(pgm, &var);
}

// variable also needs to be able to write the register back to variable
void TokenVar::putreg(Program &pgm)
{
    pgm.tkFunction->putreg(pgm.cc, &var);
}

Operand &TokenMember::operand(Program &pgm)
{
    Operand &_obj = pgm.tkFunction->voperand(pgm, &object); // make sure the parent object is all set up
    _operand = _obj.clone();
    _operand.as<x86::Mem>().setSize(var.type->size);
    _operand.as<x86::Mem>().setOffset(offset);
    return _operand; // getreg(pgm);
}

#if 0
// member needs special handling
x86::Gp &TokenMember::getreg(Program &pgm)
{
    DBG(pgm.cc.comment("TokenMember::getreg()"));
    x86::Gp &oreg = pgm.tkFunction->voperand(pgm, &object);
    _operand = var.type->newreg(pgm.cc, var.name.c_str());
#if 0
    if ( var.type->is_integer() )
    {
	DBG(pgm.cc.comment("TokenMember::getreg() xor_(_reg.r64(), _reg.r64())"));
	pgm.cc.xor_(_reg.r64(), _reg.r64());
	DBG(pgm.cc.comment("TokenMember::getreg() mtype->movrptr2rval(_reg, oreg, ofs)"));
	var.type->movrptr2rval(pgm.cc, _reg, oreg, offset);
    }
    else
#endif
    // otherwise we're using a pointer/reference (for now)
    {
	DBG(pgm.cc.comment("TokenMember::getreg() mov(_reg, oreg)"));
	pgm.cc.mov(_reg, oreg);
	pgm.cc.add(_reg, (uint64_t)offset);
    }
    return _operand.as<x86::Gp>();
}
#endif

// member also needs to be able to write the register back to variable
void TokenMember::putreg(Program &pgm)
{
    DBG(pgm.cc.comment("TokenMember::putreg()"));
    pgm.tkFunction->putreg(pgm.cc, &var);
}

Operand &TokenCallFunc::operand(Program &pgm)
{
    _operand = returns()->newreg(pgm.cc, var.name.c_str());
    DBG(pgm.cc.comment("TokenCallFunc::operand"));
    DBG(cout << "TokenCallFunc::operand() size " << _operand.size() << endl);
    return _operand;
}

Operand &TokenCallMethod::operand(Program &pgm)
{
    _operand = returns()->newreg(pgm.cc, var.name.c_str());
    return _operand;
}

#if 0
// function needs similar handling to variable
x86::Gp &TokenCallFunc::getreg(Program &pgm)
{
    _operand = returns()->newreg(pgm.cc, var.name.c_str());
    return _operand.as<x86::Gp>();
}
#endif

void TokenCpnd::movreg(x86::Compiler &cc, Operand &op, Variable *var)
{
    if ( !op.isReg() )
    {
	DBG(cc.comment("TokenCpnd::movreg() operand is not a register"));
	return;
    }
    if ( op.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	DBG(cc.comment("TokenCpnd::movreg() calling movmptr2xval(cc, reg, var->data)"));
	DBG(cc.comment(var->name.c_str()));
	var->type->movmptr2xval(cc, op.as<x86::Xmm>(), var->data);
    }
    if ( op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
    {
	DBG(cc.comment("TokenCpnd::movreg() calling movmptr2rval(cc, reg, var->data)"));
	DBG(cc.comment(var->name.c_str()));
	var->type->movmptr2rval(cc, op.as<x86::Gp>(), var->data);
    }
    else
    {
	throw "TokenCpnd::movreg() unsupported operand";
    }
}

// Manage operands/registers for use on local as well as global variables
Operand &TokenCpnd::voperand(Program &pgm, Variable *var)
{
    std::map<Variable *, Operand>::iterator rmi;

    DBG(pgm.cc.comment("TokenCpnd::voperand() on"));
    DBG(pgm.cc.comment(var->name.c_str()));

    if ( (rmi=operand_map.find(var)) != operand_map.end() )
    {
	DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::voperand(" << var->name << ") found" << std::endl);
	// copy global variable to register -- needs to happen every time we need to access a global
	if ( var->is_global() && var->data && !var->is_constant() )
	{
	    DBG(pgm.cc.comment("TokenCpnd::voperand() variable found, var->is_global() && var->data && !var->is_constant()"));
	    movreg(pgm.cc, rmi->second, var);
        }
	return rmi->second;
    }

    DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::voperand(" << var->name << ") building register" << std::endl);
    if ( (var->flags & vfSTACK) && !var->type->is_numeric() )
    {
	DBG(pgm.cc.comment("voperand on stack and non-numeric"));
	switch(var->type->type())
	{
	    case DataType::dtSTRING:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::string), 4);
		    x86::Gp reg = pgm.cc.newIntPtr(var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(std::cout << "TokenCpnd::voperand(" << var->name << ") stack var calling string_construct[" << (uint64_t)string_construct << ']' << std::endl);
		    DBG(pgm.cc.comment("string_construct"));
		    FuncCallNode *call = pgm.cc.call(imm(string_construct), FuncSignatureT<void *, void *>(CallConv::kIdHost));
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtSSTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::stringstream), 4);
		    x86::Gp reg = pgm.cc.newIntPtr(var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(pgm.cc.comment("stringstream_construct"));
		    FuncCallNode *call = pgm.cc.call(imm(stringstream_construct), FuncSignatureT<void *, void *>(CallConv::kIdHost));
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtOSTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::ostream), 4);
		    x86::Gp reg = pgm.cc.newIntPtr(var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    operand_map[var] = reg;
		}
		break;
	    default:
		if ( var->type->reftype() == RefType::rtReference
		||   var->type->reftype() == RefType::rtPointer )
		{
		    DBG(pgm.cc.comment("pgm.cc.newIntPtr"));
		    x86::Gp reg = pgm.cc.newIntPtr(var->name.c_str());
		    operand_map[var] = reg;
		    break;
		}
		if ( var->type->basetype() == BaseType::btStruct
		||   var->type->basetype() == BaseType::btClass )
		{
		    x86::Mem stack = pgm.cc.newStack(var->type->size, 4);
//		    x86::Gp reg = pgm.cc.newIntPtr(var->name.c_str());
//		    pgm.cc.lea(reg, stack);
//		    pgm.cc.mov(qword_ptr(reg, sizeof(string)), 0);
		    operand_map[var] = stack;
		    break;
		}
		std::cerr << "unsupported type: " << (int)var->type->type() << std::endl;
		std::cerr << "reftype: " << (int)var->type->reftype() << std::endl;
		throw "TokenCpnd()::voperand() unsupported type on stack";
		
	} // switch
    }
    else
    {
	DBG(pgm.cc.comment("TokenCpnd::voperand() calling var->type->newreg()"));
	operand_map[var] = var->type->newreg(pgm.cc, var->name.c_str());

	if ( (rmi=operand_map.find(var)) == operand_map.end() )
	    throw "TokenCpnd::voperand() failure";

	if ( !(var->flags & vfSTACK) )
	{
	    DBG(pgm.cc.comment("TokenCpnd::voperand() variable reg init, calling movreg on"));
	    DBG(pgm.cc.comment(var->name.c_str()));
	    movreg(pgm.cc, rmi->second, var); // first initialization of non-stack register (regset)
        }
	else
	if ( !(var->flags & vfPARAM) )
	// if it's a numeric stack register, we set it to zero, for the full size of the register
	// because subsequent operations (assignments, etc), may only access less significant
        // parts depending on the integer size, also, if we don't touch it here, we may not keep
        // access to this specific register for this variable
        {
	    if ( var->type->is_integer() )
		pgm.safexor(rmi->second, rmi->second);
	    else
	    if ( var->type->is_real() && rmi->second.isReg() && rmi->second.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
		pgm.cc.xorps(rmi->second.as<x86::Xmm>(), rmi->second.as<x86::Xmm>()); // cerr << "WARNING: floating point not initialize by voperand()" << endl;
	}
    }
    var->flags |= vfREGSET;

    if ( rmi == operand_map.end() && (rmi=operand_map.find(var)) == operand_map.end() )
	throw "TokenCpnd::voperand() failure";
    return rmi->second;
}


// only used for global varibles -- move register back into variable data
void TokenCpnd::putreg(asmjit::x86::Compiler &cc, Variable *var)
{
    // shortcut out if we can't work with this variable
    if ( !(var->is_global() && var->data && (var->flags & vfREGSET) && (var->flags & vfMODIFIED) && var->type->is_numeric()) )
	return;

    std::map<Variable *, Operand>::iterator rmi;
    if ( (rmi=operand_map.find(var)) == operand_map.end() )
    {
	std::cerr << "TokenCpnd[" << (uint64_t)this << "]::putreg(" << var->name << ") not found in operand_map" << std::endl;
	throw "TokenCpnd::setreg() called on unregistered variable";
    }

    // copy register to global variable -- needs to happen
    // every time we modify a numeric global variable
    DBG(std::cout << "TokenCpnd::putreg[" << (uint64_t)this << "](" << var->name << ") calling cc->mov(data, reg)" << std::endl);
    DBG(cc.comment("TokenCpnd::putreg() calling cc.mov(var->data, reg)"));
    if ( rmi->second.isReg() && rmi->second.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	var->type->movrval2mptr(cc, var->data, rmi->second.as<x86::Gp>());

    var->flags &= ~vfMODIFIED;
}

// cleanup function: will call destructors on all stack objects
void TokenCpnd::cleanup(asmjit::x86::Compiler &cc)
{
    std::map<Variable *, Operand>::iterator rmi;

    DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::cleanup()" << std::endl);

    for ( rmi = operand_map.begin(); rmi != operand_map.end(); ++rmi )
    {
	if ( (rmi->first->flags & vfSTACK) )
	{
	    if ( rmi->first->type->type() > DataType::dtRESERVED )
	    {
		Operand &reg = rmi->second;
		Variable *var = rmi->first;

		switch(var->type->type())
		{
		    case DataType::dtSTRING:
			{
			    DBG(std::cout << "TokenCpnd::cleanup(" << var->name << ") calling string_destruct[" << (uint64_t)string_destruct << ']' << std::endl);
			    FuncCallNode *call = cc.call(imm(string_destruct), FuncSignatureT<void, void *>(CallConv::kIdHost));
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtSSTREAM:
			{
			    FuncCallNode *call = cc.call(imm(stringstream_destruct), FuncSignatureT<void, void *>(CallConv::kIdHost));
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtOSTREAM:
			{
			    FuncCallNode *call = cc.call(imm(ostream_destruct), FuncSignatureT<void, void *>(CallConv::kIdHost));
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    default:
			DBG(std::cerr << "Unable to handle stack variable: " << var->type->name << ' ' << var->name << " type: " << (int)var->type->type() << std::endl);
			break;
		} // switch
	    }
	}
    }
}

#if 0
// Keyword handlers
void Program::compileKeyword(TokenKeyword *tk)
{
    DBG(cout << "compileKeyword() " << tk->str << ')' << endl);
    tk->compile(*this);
}
#endif

/////////////////////////////////////////////////////////////////////////////
// mathematical operators                                                  //
/////////////////////////////////////////////////////////////////////////////

void TokenOperator::setregdp(Program &pgm, regdefp_t &regdp)
{
    if ( left->type() == TokenType::ttReal || right->type() == TokenType::ttReal )
    {
	if ( !regdp.second )
	    regdp.second = &ddDOUBLE;
	if ( regdp.first )
	    return;
	_operand = pgm.cc.newXmm("setregdp_xmm");
	regdp.first = &_operand;
	return;
    }
    if ( left->type() == TokenType::ttInteger || right->type() == TokenType::ttInteger )
    {
	if ( !regdp.second )
	{
	    DBG(pgm.cc.comment("setregdp() regdp.second = &ddINT"));
	    regdp.second = &ddINT;
	}
	if ( regdp.first )
	    return;
	_operand = pgm.cc.newGpq("setregdp_reg");
	regdp.first = &_operand;
	return;
    }
}

// set regdp.second (type)
void TokenOperator::settype(Program &pgm, regdefp_t &regdp)
{
    if ( regdp.second )
	return;
    if ( (left && left->is_real()) || (right && right->is_real()) )
	regdp.second = &ddDOUBLE;
    else
    if ( (left && left->datadef()->is_integer() ) )
	regdp.second = left->datadef();
    else
    if ( (right && right->datadef()->is_integer() ) )
	regdp.second = right->datadef();
    else
    {
	DBG(pgm.cc.comment("settype() regdp.second = &ddINT"));
	regdp.second = &ddINT;
    }
}

/////////////////////////////////////////////////////////////////////////////
//                                                                         //
// Check to see if we can optimize the current operation.                  //
//                                                                         //
// This is known as "constant folding"                                     //
//                                                                         //
/////////////////////////////////////////////////////////////////////////////

bool TokenOperator::can_optimize()
{
    if ( (!left || left->is_constant()) && (!right || right->is_constant()) )
	return true;
    return false;
}


/////////////////////////////////////////////////////////////////////////////
//                                                                         //
// Optimize a standard operation which has a constant value on both sides  //
//                                                                         //
// This is known as "constant folding"                                     //
//                                                                         //
// Currently we only handle a single level, but ideally we should fold     //
// all constant expressions together (todo later)                          //
//                                                                         //
/////////////////////////////////////////////////////////////////////////////

Operand &TokenOperator::optimize(Program &pgm, regdefp_t &regdp)
{
    if ( (regdp.second && regdp.second->is_real())
    ||   (left  && left->is_real())
    ||   (right && right->is_real()) )
    {
	if ( !regdp.second ) { regdp.second = &ddDOUBLE; }
	if ( !regdp.first )
	{
	    _operand = pgm.cc.newXmm("_operand_Xmm_");
	    regdp.first = &_operand;
	}
	pgm.safemov(*regdp.first, foperate(), regdp.second);
	return *regdp.first;
    }
    if ( !regdp.second )
    {
	DBG(pgm.cc.comment("optimize() regdp.second = &ddINT"));
	regdp.second = &ddINT;
    }
    if ( !regdp.first )
    {
	_operand = pgm.cc.newGpq("_operand_Gpq_");
	regdp.first = &_operand;
    }
    pgm.safemov(*regdp.first, ioperate(), regdp.second);
    return *regdp.first;
}


/////////////////////////////////////////////////////////////////////////////
//                                                                         //
// logic for operators:                                                    //
//                                                                         //
// First we need to check if we have an appropriate left and/or right as   //
// needed, and then we need to check if we need to operate on a variable   //
// (=, ++, --, +=, *=, /=, ^=, etc), if not, then we need to see if both   //
// sides are constant/static, and if so, we can shortcut, computing the    //
// result at compile time, and using it to return the correct operand.     //
//                                                                         //
// If only one side is constant, we need to compile() the non-const side   //
// first, to the left side, and then bring the constant in for the right,  //
// otherwise we compile() the right side. We then perform the operation in //
// code, and return the resulting operand in the correct form.             //
//                                                                         //
// TODO: need to support operator overloading                              //
//                                                                         //
/////////////////////////////////////////////////////////////////////////////


// addition
Operand &TokenAdd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAdd::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "+ missing lval operand"; }
    if ( !right ) { throw "+ missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenAdd::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safeadd(lval, rval, regdp.second);		 // type safe addition
    regdp.first = &lval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// subtraction
Operand &TokenSub::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenSub::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "- missing lval operand"; }
    if ( !right ) { throw "- missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenAdd::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safesub(lval, rval, regdp.second);		 // type safe subtraction
    regdp.first = &lval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// make number negative
Operand &TokenNeg::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenNeg::Compile() TOP" << endl);
    if ( !right ) { throw "- missing rval operand"; }
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &rval = right->compile(pgm, regdp);		 // compile right side ref=rval
    if ( !regdp.second ) { throw "TokenNeg::compile() right->compile cleared datatype"; }
    pgm.safeneg(rval);					 // type safe negation
    regdp.first = &rval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// multiply two numbers
Operand &TokenMul::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMul::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "* missing lval operand"; }
    if ( !right ) { throw "* missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenMul::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safemul(lval, rval, regdp.second);		 // type safe multiplication
    regdp.first = &lval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// divide two numbers
Operand &TokenDiv::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDiv::Compile() TOP" << endl);
    if ( !left )  { throw "/ missing lval operand"; } 
    if ( !right ) { throw "/ missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand remainder = regdp.second->newreg(pgm.cc, "remainder");
    Operand &dividend = left->compile(pgm, regdp);	 // compile left side ref=dividend
    if ( !regdp.second ) { throw "TokenDiv::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "divisor");// use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &divisor = right->compile(pgm, regdp);	 // compile right side into tmp
    pgm.safexor(remainder, remainder);			 // zero out remainder
    pgm.safediv(remainder, dividend, divisor, regdp.second);// type safe division
    regdp.first = &dividend;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

#if 0
// divide two numbers
Operand &TokenDiv::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDiv::Compile() TOP" << endl);
    if ( !left )  { throw "/ missing lval operand"; } 
    if ( !right ) { throw "/ missing rval operand"; }
    if ( can_optimize() )  {return optimize(pgm, regdp);} 
    Operand remainder = pgm.cc.newInt64("TokenDiv::remainder");
    DBG(pgm.cc.comment("TokenDiv::compile() left->compile()"));
    Operand &dividend = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenDiv::compile() left->compile didn't set datatype"; }
    DBG(pgm.cc.comment("TokenDiv::compile() regdp.second->newreg(divisor)"));
    _operand = regdp.second->newreg(pgm.cc, "divisor"); // use tmp for right side
    regdp.first = &_operand;
    DBG(cout << "TokenDiv::compile() right->compile()" << endl);
    DBG(pgm.cc.comment("TokenDiv::compile() right->compile()"));
    Operand &divisor = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenDiv::compile() safexor()"));
    pgm.safexor(remainder, remainder);
    DBG(pgm.cc.comment("TokenDiv::compile() pgm.safediv(remainder, _reg, rval)"));
    pgm.safediv(remainder, dividend, divisor);
    regdp.first = &dividend;
    return *regdp.first;
}
#endif

// modulus
Operand &TokenMod::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMod::Compile() TOP" << endl);
    if ( !left )  { throw "% missing lval operand"; }
    if ( !right ) { throw "% missing rval operand"; }
    if ( can_optimize() )  {return optimize(pgm, regdp);} 

    if ( !regdp.first ) // { throw "% missing register"; }
    {
	_operand = pgm.cc.newInt64("remainder");
	regdp.first = &_operand;
    }
    Operand &remainder = *regdp.first;
    Operand _dividend;
    if ( regdp.second )
	_dividend = regdp.second->newreg(pgm.cc, "dividend");
    else
    {
	_dividend = pgm.cc.newInt64("dividend");
	DBG(pgm.cc.comment("TokenMod() regdp.second = &ddINT"));
	regdp.second = &ddINT;
    }
    regdp.first = &_dividend;
    Operand &dividend = left->compile(pgm, regdp);
    Operand divisor = regdp.second->newreg(pgm.cc, "divisor");
    regdp.first = &divisor;
    right->compile(pgm, regdp);
    pgm.safexor(remainder, remainder); // clear whole register
    DBG(pgm.cc.comment("TokenMod::compile() pgm.cc.idiv(remainder, lreg, rval)"));
    pgm.safediv(remainder, dividend, divisor);
    regdp.first = &remainder;
    return *regdp.first;
}
/////////////////////////////////////////////////////////////////////////////
// bit math operators                                                      //
/////////////////////////////////////////////////////////////////////////////

// bit shift left
Operand &TokenBSL::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBSL::Compile() TOP" << endl);
    if ( !left )  { throw "<< missing lval operand"; }
    if ( !right ) { throw "<< missing rval operand"; }
    if ( can_optimize() )  {return optimize(pgm, regdp);} 

    // hard coding some basic ostream support for now, will use operator overloading later
    if ( left->type() == TokenType::ttVariable && dynamic_cast<TokenVar *>(left)->var.type->has_ostream() )
    {
	TokenVar *tvl = dynamic_cast<TokenVar *>(left);
	DBG(pgm.cc.comment("TokenBSL::compile() (ostream &)tvl->getreg(pgm)"));
	Operand &lval = tvl->operand(pgm); // get ostream register

	if ( !lval.isReg() )
	    throw "TokenBSL::compile() tval operand not a register";

	DBG(cout << "TokenBSL::compile() lval(" << tvl->var.name << ")->has_ostream()" << endl);

	// converge streams
	if ( right->id() == TokenID::tkBSL && !right->is_bracketed() )
	{
	    DBG(cout << "TokenBSL::compile() converging right BSL(<<) to left ostream" << endl);
	    TokenBSL tmpsin;
	    TokenBSL *rsin = static_cast<TokenBSL *>(right);
	    tmpsin.left = left;
	    tmpsin.right = rsin->left;
	    tmpsin.compile(pgm, regdp);
	    tmpsin.right = rsin->right;
	    tmpsin.compile(pgm, regdp);
	    DBG(cout << "TokenBSL::Compile() END" << endl);
	    regdp.first = &lval;
	    regdp.second = tvl->var.type;
	    return *regdp.first; // return ostream
	}

	// handle ostreaming
//	regdp.first  = &lval; // pass along the ostream?
//	regdp.second = tvl->var.type;
	regdp.first = NULL;
	regdp.second = NULL;
	regdp.object = &lval;
	DBG(cout << "TokenBSL::compile() calling right->compile() on " << (int)right->type() << endl);
	/* Operand &rval =*/ right->compile(pgm, regdp); // compile right side

	if ( !regdp.second )
	{
	    cerr << "TokenBSL::compile() right->type() " << (int)right->type() << " right->id() " << (int)right->id() << endl;
	    throw "TokenBSL::compile() unable to determine rval type";
	}

	// returns ostream? do nothing, it's already done
	if ( regdp.second->has_ostream() )
	{
	    DBG(cout << "TokenBSL::compile() regdp.second->has_ostream()" << endl);
	}
	else
	if ( regdp.second->is_numeric() )
	{
	    DBG(cout << "TokenBSL::compile() regdp.second->is_numeric()" << endl);
	    DBG(pgm.cc.comment("pgm.cc.call(streamout_numeric)"));
	    FuncCallNode *call;
	    switch(regdp.second->type())
	    {
		case DataType::dtCHAR:	call = pgm.cc.call(imm(streamout_numeric<char>), FuncSignatureT<void, void *, char>(CallConv::kIdHost));	break;
		case DataType::dtBOOL:	call = pgm.cc.call(imm(streamout_numeric<bool>), FuncSignatureT<void, void *, bool>(CallConv::kIdHost));	break;
		case DataType::dtINT16:	call = pgm.cc.call(imm(streamout_numeric<int16_t>), FuncSignatureT<void, void *, int16_t>(CallConv::kIdHost));	break;
		case DataType::dtINT24:	call = pgm.cc.call(imm(streamout_numeric<int16_t>), FuncSignatureT<void, void *, int16_t>(CallConv::kIdHost));	break;
		case DataType::dtINT32:	call = pgm.cc.call(imm(streamout_numeric<int32_t>), FuncSignatureT<void, void *, int32_t>(CallConv::kIdHost));	break;
		case DataType::dtINT64:	call = pgm.cc.call(imm(streamout_numeric<int64_t>), FuncSignatureT<void, void *, int64_t>(CallConv::kIdHost));	break;
		case DataType::dtUINT8:	call = pgm.cc.call(imm(streamout_numeric<uint8_t>), FuncSignatureT<void, void *, uint8_t>(CallConv::kIdHost));	break;
		case DataType::dtUINT16:call = pgm.cc.call(imm(streamout_numeric<uint16_t>), FuncSignatureT<void, void *, uint16_t>(CallConv::kIdHost));break;
		case DataType::dtUINT24:call = pgm.cc.call(imm(streamout_numeric<uint16_t>), FuncSignatureT<void, void *, uint16_t>(CallConv::kIdHost));break;
		case DataType::dtUINT32:call = pgm.cc.call(imm(streamout_numeric<uint32_t>), FuncSignatureT<void, void *, uint32_t>(CallConv::kIdHost));break;
		case DataType::dtUINT64:call = pgm.cc.call(imm(streamout_numeric<uint64_t>), FuncSignatureT<void, void *, uint64_t>(CallConv::kIdHost));break;
		case DataType::dtFLOAT: DBG(pgm.cc.comment("pgm.cc.call(imm(streamout_numeric<float>),  FuncSignatureT<void, void *, float>(CallConv::kIdHost))"));
		call = pgm.cc.call(imm(streamout_numeric<float>),  FuncSignatureT<void, void *, float>(CallConv::kIdHost));	break;
		case DataType::dtDOUBLE: DBG(pgm.cc.comment("pgm.cc.call(imm(streamout_numeric<double>), FuncSignatureT<void, void *, double>(CallConv::kIdHost))"));
		call = pgm.cc.call(imm(streamout_numeric<double>), FuncSignatureT<void, void *, double>(CallConv::kIdHost));	break;
		default: throw "TokenBSL::compile() unsupported numeric type";
	    }
	    DBG(pgm.cc.comment("about to setArg(0)"));
	    if ( lval.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    {
		DBG(pgm.cc.comment("call->setArg(0, lval.as<x86::Xmm>())"));
		call->setArg(0, lval.as<x86::Xmm>());
	    }
	    else
	    if ( lval.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    {
		DBG(pgm.cc.comment("call->setArg(0, lval.as<x86::Gp>())"));
		DBG(cout << "call->setArg(0, lval.as<x86::Gp>()) size=" << lval.size() << " regdp.second->size=" << regdp.second->size << " type " << regdp.second->name << endl);
		call->setArg(0, lval.as<x86::Gp>());
	    }
	    else
		throw "TokenBSL::compile() lval unsupported register type";

	    if ( regdp.first->isReg() )
	    {
		if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupVec) )
		{
		    DBG(cout << "call->setArg(1, regdp.first->as<x86::Xmm>()) size=" << regdp.first->size() << " regdp.second->size=" << regdp.second->size << " type " << regdp.second->name << endl);
		    DBG(pgm.cc.comment("call->setArg(1, regdp.first->as<x86::Xmm>())"));
		    call->setArg(1, regdp.first->as<x86::Xmm>());
		}
		else
		if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupGp) )
		    call->setArg(1, regdp.first->as<x86::Gp>());
		else
		    throw "TokenBSL::compile() unexpected parameter Operand";
	    }
	    else
	    if ( regdp.first->isImm() )
		call->setArg(1, regdp.first->as<Imm>());
	}
	else
	if ( regdp.second->is_string() )
	{
	    if ( !regdp.first ) { pgm.Throw(this) << "TokenBSL::compile() regdp.first is NULL" << flush; }
	    if ( !regdp.first->isReg() && !regdp.first->isMem() )
	    {
		pgm.Throw(this) << "TokenBSL::compile() regdp.first->isReg() is FALSE" << flush;
	    }
	    if ( regdp.first->isReg() && !regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupGp) ) { throw "TokenBSL::compile() regdp.first not GpReg"; }
	    DBG(cout << "TokenBSL::compile() regdp.second->is_string()" << endl);
	    DBG(pgm.cc.comment("TokenBSL::compile() regdp.second->is_string()"));
	    DBG(pgm.cc.comment("pgm.cc.call(streamout_string)"));
	    FuncCallNode* call = pgm.cc.call(imm(streamout_string), FuncSignatureT<void, void *, void *>(CallConv::kIdHost));
	    DBG(pgm.cc.comment("call->setArg(0, lval)"));
	    if ( lval.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
		call->setArg(0, lval.as<x86::Xmm>());
	    else
	    if ( lval.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
		call->setArg(0, lval.as<x86::Gp>());
	    else
		throw "TokenBSL::compile() lval unsupported register type";
	    DBG(pgm.cc.comment("call->setArg(1, rval)"));
	    call->setArg(1, regdp.first->as<x86::Gp>());
	}
	else
	if ( regdp.second->type() == DataType::dtCHARptr )
	{
	    if ( !regdp.first ) { pgm.Throw(this) << "TokenBSL::compile() regdp.first is NULL" << flush; }
	    if ( !regdp.first->isReg() && !regdp.first->isMem() )
	    {
		pgm.Throw(this) << "TokenBSL::compile() regdp.first->isReg() is FALSE" << flush;
	    }
	    if ( regdp.first->isReg() && !regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupGp) ) { throw "TokenBSL::compile() regdp.first not GpReg"; }
	    DBG(cout << "TokenBSL::compile() regdp.second->is_cstr()" << endl);
	    DBG(pgm.cc.comment("TokenBSL::compile() regdp.second->is_cstr()"));
	    DBG(pgm.cc.comment("pgm.cc.call(streamout_cstr)"));
	    FuncCallNode* call = pgm.cc.call(imm(streamout_cstr), FuncSignatureT<void, void *, void *>(CallConv::kIdHost));
	    DBG(pgm.cc.comment("call->setArg(0, lval)"));
	    if ( lval.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
		call->setArg(0, lval.as<x86::Xmm>());
	    else
	    if ( lval.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
		call->setArg(0, lval.as<x86::Gp>());
	    else
		throw "TokenBSL::compile() lval unsupported register type";
	    DBG(pgm.cc.comment("call->setArg(1, rval)"));
	    call->setArg(1, regdp.first->as<x86::Gp>());
	}
	else
	{
	    cerr << "TokenBSL::compile() regdp.second.name: " << regdp.second->name << " regdp.second->type() " << (int)regdp.second->type() << endl;
	    pgm.Throw(this) << "TokenBSL::compile unsupported dataype " << regdp.second->name << " (" << (int)regdp.second->type() << ')' << flush;
	}

	DBG(cout << "TokenBSL::Compile() END" << endl);
	regdp.first = &lval;
	regdp.second = tvl->var.type;

	return *regdp.first; // return ostream
    }

    // handle left bitshift

    DBG(cout << "TokenBSL::compile() left->type() == " << (int)left->type()  << endl);
    DBG(cout << "TokenBSL::compile() right->type() == " << (int)right->type()  << endl);

    if ( left->type() == TokenType::ttVariable && !dynamic_cast<TokenVar *>(left)->var.type->is_numeric() )
	throw "lval is non-numeric";
    if ( right->type() == TokenType::ttVariable && !dynamic_cast<TokenVar *>(right)->var.type->is_numeric() )
	throw "rval is non-numeric";
//  if ( !regdp.first ) { throw "<< missing register"; }

    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenBSL::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safeshl(lval, rval);				 // type safe shift left
    regdp.first = &lval;				 // restore regdp.first
    DBG(cout << "TokenBSL::Compile() END" << endl);	 // (debugging message)
    return *regdp.first;				 // return result operand
}

// bit shift right
Operand &TokenBSR::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBSR::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenBSR::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safeshr(lval, rval);				 // type safe shift right
    regdp.first = &lval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// bitwise or |
Operand &TokenBor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenBor::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safeor(lval, rval);				 // type safe binary or
    regdp.first = &lval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// bitwise xor ^
Operand &TokenXor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenXor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenXor::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safexor(lval, rval);				 // type safe exclusive or
    regdp.first = &lval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// bitwise and &
Operand &TokenBand::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenBand::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.safeand(lval, rval);				 // type safe binary and
    regdp.first = &lval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// bitwise not ~
Operand &TokenBnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBnot::Compile() TOP" << endl);
    if ( left )   { throw "Bitwise not has lval!"; }
    if ( !right ) { throw "~ missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &rval = right->compile(pgm, regdp);		 // compile right side ref=rval
    if ( !regdp.second ) { throw "TokenBnot::compile() right->compile cleared datatype"; }
    pgm.safenot(rval);					 // type safe bitwise not
    regdp.first = &rval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

/////////////////////////////////////////////////////////////////////////////
// logic operators                                                         //
/////////////////////////////////////////////////////////////////////////////

// logical not !
Operand &TokenLnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLnot::Compile(" << (regdp.first ? "first" : "") << ") TOP" << endl);
    if ( left )   { throw "! unexpected lval!"; }
    if ( !right ) { throw "! missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &rval = right->compile(pgm, regdp);		 // compile right side ref=rval
    if ( !regdp.second ) { throw "TokenLnot::compile() right->compile cleared datatype"; }
    DBG(cout << "TokenLnot::compile() pgm.safetest(rval, rval)" << endl);
    DBG(pgm.cc.comment("TokenLnot::compile() pgm.safetest(rval, rval)"));
    pgm.testzero(rval);					 // test rval is 0
    DBG(pgm.cc.comment("TokenLnot::compile() pgm.cc.sete(regdp.first)"));
    pgm.safesete(*regdp.first);
    return *regdp.first;
}


// logical or ||
//
// Pseudocode: if (lval) return 1;  if (rval) return 1;  return 0;
//
Operand &TokenLor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLor::Compile() TOP" << endl);
    DBG(pgm.cc.comment("TokenLor::compile() TOP"));
    if ( !left )  { throw "|| missing lval operand"; }
    if ( !right ) { throw "|| missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    Label done = pgm.cc.newLabel();			 // label to skip further tests
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenLor _operand = newreg"));
	DBG(cout << "TokenLor _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenLor::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.testzero(lval);					 // test lval is 0
    pgm.safesetne(lval);				 // if lval !=0, ret(lval) = 1
    pgm.cc.jne(done);					 // if lval != 0, jump to done
    pgm.testzero(rval);					 // test rval is 0
    pgm.safesetne(lval);				 // if rval !=0, ret(lval) = 1
    pgm.cc.bind(done);					 // done is here
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}

// logical and &&
//
// Pseudocode: if (!lval) return 0;  if (!rval) return 0;  return 1;
//
Operand &TokenLand::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLand::Compile() TOP" << endl);
    DBG(pgm.cc.comment("TokenLand::compile() TOP"));
    if ( !left )  { throw "&& missing lval operand"; }
    if ( !right ) { throw "&& missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    Label done = pgm.cc.newLabel();			 // label to skip further tests
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenLand _operand = newreg"));
	DBG(cout << "TokenLand _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenLand::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    pgm.testzero(lval);					 // test lval is 0
    pgm.safesetne(lval);				 // if lval !=0, ret(lval) = 1
    pgm.cc.je(done);					 // if lval == 0, jump to done
    pgm.testzero(rval);					 // test rval is 0
    pgm.safesetne(lval);				 // if rval !=0, ret(lval) = 1
    pgm.cc.bind(done);					 // done is here
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}


/////////////////////////////////////////////////////////////////////////////
// comparison operators                                                    //
/////////////////////////////////////////////////////////////////////////////


// Equal to: ==
Operand &TokenEquals::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenEquals::Compile() TOP" << endl);
    if ( !left )  { throw "== missing lval operand"; }
    if ( !right ) { throw "== missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenEquals _operand = newreg"));
	DBG(cout << "TokenEquals _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenEquals::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);				 // typesafe comparison
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.safesete(reg)"));
    pgm.safesete(lval);					 // if lval == rval, ret(lval) = 1
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}

// Not equal to: !=
Operand &TokenNotEq::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenNotEq::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenNotEq _operand = newreg"));
	DBG(cout << "TokenNotEq _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenNotEq::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);				 // typesafe comparison
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.safesetne(reg)"));
    pgm.safesetne(lval);				 // if lval != rval, ret(lval) = 1
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}

// Less than: <
Operand &TokenLT::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLT::Compile() TOP" << endl);
    if ( !left )  { throw "== missing lval operand"; }
    if ( !right ) { throw "== missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenLT _operand = newreg"));
	DBG(cout << "TokenLT _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenLT::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    DBG(pgm.cc.comment("TokenLT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);				 // typesafe comparison
    DBG(pgm.cc.comment("TokenLT::compile() pgm.safesetl(reg)"));
    pgm.safesetl(lval);					 // if lval < rval, ret(lval) = 1
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}

// Less than or equal to: <=
Operand &TokenLE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLE::Compile() TOP" << endl);
    if ( !left )  { throw "== missing lval operand"; }
    if ( !right ) { throw "== missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenLE _operand = newreg"));
	DBG(cout << "TokenLE _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenLE::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    DBG(pgm.cc.comment("TokenLE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);				 // typesafe comparison
    DBG(pgm.cc.comment("TokenLE::compile() pgm.safesetle(reg)"));
    pgm.safesetle(lval);				 // if lval <= rval, ret(lval) = 1
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}

// Greater than: >
Operand &TokenGT::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenGT::Compile() TOP" << endl);
    if ( !left )  { throw "== missing lval operand"; }
    if ( !right ) { throw "== missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenGT _operand = newreg"));
	DBG(cout << "TokenGT _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenGT::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    DBG(pgm.cc.comment("TokenGT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);				 // typesafe comparison
    DBG(pgm.cc.comment("TokenGT::compile() pgm.safesetg(reg)"));
    pgm.safesetg(lval);					 // if lval > rval, ret(lval) = 1
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}

// Greater than or equal to: >=
Operand &TokenGE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenGE::Compile() TOP" << endl);
    if ( !left )  { throw "== missing lval operand"; }
    if ( !right ) { throw "== missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("TokenGE _operand = newreg"));
	DBG(cout << "TokenGE _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "TokenGE::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    DBG(pgm.cc.comment("TokenGE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);				 // typesafe comparison
    DBG(pgm.cc.comment("TokenGE::compile() pgm.safesetge(reg)"));
    pgm.safesetge(lval);				 // if lval >= rval, ret(lval) = 1
    regdp.first = &lval;				 // set regdp.first to lval
    return *regdp.first;				 // return result operand(lval)
}


// Greater than gives 1, less than gives -1, equal to gives 0 (<=>)
Operand &Token3Way::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "Token3Way::Compile() TOP" << endl);
    Label done = pgm.cc.newLabel();	// label to skip further tests
    Label sign = pgm.cc.newLabel();	// label to negate _reg (make negative)
    if ( !left )  { throw "<=> missing lval operand"; }
    if ( !right ) { throw "<=> missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	DBG(pgm.cc.comment("Token3Way _operand = newreg"));
	DBG(cout << "Token3Way _operand = newreg" << endl);
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &lval = left->compile(pgm, regdp);		 // compile left side ref=lval
    if ( !regdp.second ) { throw "Token3Way::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");   // use tmp for right side
    regdp.first = &tmp;					 // pass tmp along
    Operand &rval = right->compile(pgm, regdp);		 // compile right side into tmp
    DBG(pgm.cc.comment("Token3Way::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);				 // typesafe comparison

    pgm.safesetg(lval);					 // set lval to 1 if >
    pgm.cc.jg(done);					 // if >, jump to done
    pgm.safesetl(lval);					 // set lval to 1 if <
    pgm.cc.jl(sign);					 // if <, jump to negate
    pgm.safexor(lval, lval);				 // lval = 0
    pgm.cc.bind(sign);
    pgm.safeneg(lval);					 // _lval ? 1 : -1
    pgm.cc.bind(done);					 // done
    return *regdp.first;
}

// access structure/class member: struct.member
Operand &TokenDot::compile(Program &pgm, regdefp_t &regdp)
{
#if 0
    DBG(cout << "TokenDot::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( left->type() != TokenType::ttVariable )
	throw "Accessing on a non-variable lval";
    if ( right->type() != TokenType::ttIdentifier )
	throw "Was expecting rval to be identifier";

    TokenVar *tvl = dynamic_cast<TokenVar *>(left);
    if ( !tvl->var.type->is_struct() && !tvl->var.type->is_object() )
	throw "Expecting class or structure";
    TokenIdent *tvr = static_cast<TokenIdent *>(right);
    Variable *classmethod = NULL;
    DBG(cout << "TokenDot::compile() accessing " << tvl->var.name << '.' << tvr->str << endl);
    // if class, try for member
    if ( tvl->var.type->is_object() && (classmethod=((DataDefCLASS *)tvl->var.type)->findMethod(tvr->str)) )
    {
	cout << "Found " << tvl->var.name << "::" << classmethod->name << endl;
	throw "TokenDot::compile() found method :)";
    }
    // get offset
    ssize_t ofs = ((DataDefSTRUCT *)tvl->var.type)->m_offset(tvr->str);
    if ( ofs == -1 )
	throw "Unidentified member";
    // get left register
    DBG(pgm.cc.comment("TokenDot::compile() tvl->operand(pgm)"));
    Operand &lval = tvl->operand(pgm);
    DataDef *mtype = ((DataDefSTRUCT *)tvl->var.type)->m_type(tvr->str);
    DBG(pgm.cc.comment("TokenDot::compile() _reg= mtype->newreg(tvr->str)"));
    // get new register of appropriate size
    _operand = mtype->newreg(pgm.cc, tvr->str.c_str());
    // if it's numeric, clear out the full register, then copy the data over
    if ( mtype->is_numeric() )
    {
	DBG(pgm.cc.comment("TokenDot::compile() xor_(_reg.r64(), _reg.r64())"));
	pgm.cc.xor_(_reg.r64(), _reg.r64());
	DBG(pgm.cc.comment("TokenDot::compile() mtype->movrptr2rval(_reg, lval, ofs)"));
	mtype->movrptr2rval(pgm.cc, _reg, lval, ofs);
    }
    else
    // otherwise we're using a pointer/reference (for now)
    {
	DBG(pgm.cc.comment("TokenDot::compile() mov(_reg, lval)"));
	pgm.cc.mov(_reg, lval);
	pgm.cc.add(_reg, (uint64_t)ofs);
    }

    regdp.first  = &_operand;
    regdp.second = mtype;
    DBG(pgm.cc.comment("TokenDot::compile() mtype->name:"));
    DBG(pgm.cc.comment(mtype->name.c_str()));
#endif
    return _operand;
}


// load variable into register
Operand &TokenVar::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenVar::compile() reg = operand()"));
    Operand &reg = operand(pgm);

    if ( !regdp.second )
	regdp.second = _datatype;

    DBG(cout << "TokenVar::compile() name=" << var.name << " regdp.second.name " << regdp.second->name << endl);

    if ( regdp.first )
    {
	if ( !reg.isEqual(*regdp.first) && regdp.first != &reg )
	{
	    DBG(pgm.cc.comment("TokenVar::compile() safemov(*ret, reg)"));
	    pgm.safemov(*regdp.first, reg, regdp.second, var.type);
	}
	return *regdp.first;
    }
    if ( reg.size() < 4 && reg.size() < regdp.second->size )
    {
	DBG(cout << "TokenVar::compile() reg.size() " << reg.size() << " < regdp.second->size " << regdp.second->size << endl);
	if ( regdp.second->is_integer() && var.type->is_integer() )
	{
	    DBG(pgm.cc.comment("TokenVar::compile() safeextend(reg, is_unsigned())"));
	    pgm.safeextend(reg, var.type->is_unsigned());
	}
    }

    regdp.first = &reg;
    return reg;
}

// load variable into register
Operand &TokenMember::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenMember::compile() reg = operand()"));
    Operand &reg = operand(pgm);

    if ( !regdp.second )
	regdp.second = _datatype;

    if ( regdp.first )
    {
	DBG(pgm.cc.comment("TokenMember::compile() safemov(*ret, reg)"));
	pgm.safemov(*regdp.first, reg, regdp.second);
	return *regdp.first;
    }
    regdp.first = &reg;

    return reg;
}

// load double into operand
Operand &TokenReal::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenReal::compile()"));
    if ( !regdp.second )
    {
	if ( !_datatype ) { throw "TokenReal has NULL _datatype"; }
	regdp.second = _datatype;
	DBG(pgm.cc.comment("TokenReal::compile() setting _datatype to double"));
    }
    if ( regdp.first )
    {
	_const = pgm.cc.newDoubleConst(ConstPool::kScopeLocal, _val);
	DBG(pgm.cc.comment("TokenReal::compile() calling safemov(*regdp.first, _const)"));
	DBG(cout << "TokenReal::compile() calling safemov(*regdp.first, _const[" << _val << "])" << endl);
	pgm.safemov(*regdp.first, _const, regdp.second);
    }
    else
    {
	_operand = operand(pgm);
	regdp.first = &_operand;
    }
    return *regdp.first;
}

// load integer into register
Operand &TokenInt::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !regdp.second )
    {
	if ( !_datatype ) { throw "TokenInt has NULL _datatype"; }
	regdp.second = _datatype;
	DBG(pgm.cc.comment("TokenInt::compile() setting _datatype to int"));
    }
    if ( regdp.first )
    {
	DBG(pgm.cc.comment("TokenInt::compile() cc.mov(*ret, value)"));
	pgm.safemov(*regdp.first, _token, regdp.second);
	return *regdp.first;
    }
    DBG(cout << "TokenInt::compile[" << (uint64_t)this << "]() value: " << (int)_token << endl);
    regdp.first = &_operand;
    return operand(pgm);
}

// compile a return statement
Operand &TokenRETURN::compile(Program &pgm, regdefp_t &regdp)
{
    pgm.tkFunction->cleanup(pgm.cc);

    if ( returns )
    {
	Operand &reg = returns->compile(pgm, regdp);
	pgm.saferet(reg);
	return reg;
    }
    pgm.cc.ret();

    return _reg;
}

// compile a break statement
Operand &TokenBREAK::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBREAK::compile(pgm)");
    if ( !pgm.loopstack.empty() )
    {
	DBG(pgm.cc.comment("BREAK"));
	pgm.cc.jmp(*pgm.loopstack.top().second);
    }
    return _reg;
}

// compile a continue statement
Operand &TokenCONT::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !pgm.loopstack.empty() )
    {
	DBG(pgm.cc.comment("CONTINUE"));
	pgm.cc.jmp(*pgm.loopstack.top().first);
    }
    return _reg;
}

// compile an if statement
Operand &TokenIF::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenIF::compile() TOP" << std::endl);
    Label iftail = pgm.cc.newLabel();	// label for tail of if
    Label thendo = pgm.cc.newLabel();	// label for then condition
    Label elsedo = pgm.cc.newLabel();	// label for else condition

    if ( !statement ) { throw "if missing statement"; }
    // push labels onto ifstack
    pgm.ifstack.push(make_pair(&thendo, elsestmt ? &elsedo : &iftail));
    // perform condition check, false goes either to elsedo or iftail
    DBG(pgm.cc.comment("TokenIF::compile() reg = condition->compile()"));
    Operand &reg = condition->compile(pgm, regdp);
    // hard coded if (1) / if (0)
    if ( reg.isImm() )
    {
	// if (1) (or any non-zero)
	if ( reg.as<Imm>().i64() )
	{
	    pgm.cc.bind(thendo);
	    DBG(pgm.cc.comment("TokenIF::compile(1) statement->compile(pgm, regdp)"));
	    statement->compile(pgm, regdp); // execute if statement(s) for true
	}
	else
	// if (0)
	if ( elsestmt )
	{
	    pgm.cc.bind(elsedo);	// bind elsedo label
	    DBG(pgm.cc.comment("TokenIF::compile(0) elsestmt->compile(pgm, regdp)"));
	    elsestmt->compile(pgm, regdp);  // execute else condition
	}
    }
    // logic controlled
    else
    if ( reg.isReg() || reg.isMem() )
    {
	DBG(cout << "TokenIF::compile() pgm.safetest(reg, reg)" << endl);
	DBG(pgm.cc.comment("TokenIF::compile() pgm.safetest(reg, reg)"));
	pgm.testzero(reg); //pgm.safetest(reg, reg);			// compare to zero
	DBG(pgm.cc.comment("TokenIF::compile() pgm.cc.je(else/tail)"));
	pgm.cc.je(elsestmt ? elsedo : iftail);	// jump appropriately

	DBG(pgm.cc.comment("TokenIF::compile() statement->compile(pgm, regdp)"));
	pgm.cc.bind(thendo);
	statement->compile(pgm, regdp); // execute if statement(s) if condition met
	if ( elsestmt )			// do we have an else?
	{
	    pgm.cc.jmp(iftail);		// jump to tail after executing if statements
	    pgm.cc.bind(elsedo);	// bind elsedo label
	    DBG(pgm.cc.comment("TokenIF::compile() elsestmt->compile(pgm, regdp)"));
	    elsestmt->compile(pgm, regdp); 	// execute else condition
	}
    }
    else
	throw "TokenIF::compile() condition->compile() didn't return a usable operand";
    pgm.cc.bind(iftail);		// bind if tail

    pgm.ifstack.pop();			// pop labels from ifstack
    DBG(std::cout << "TokenIF::compile() END" << std::endl);

    return reg;
}

Operand &TokenDO::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenDO::compile() TOP" << std::endl);
    Label dotop  = pgm.cc.newLabel();	// label for top of loop
    Label dodo   = pgm.cc.newLabel();	// label for loop action
    Label dotail = pgm.cc.newLabel();	// label for tail of loop

    pgm.loopstack.push(make_pair(&dotop, &dotail)); // push labels onto loopstack
    pgm.cc.bind(dotop);			// label the top of the loop
    DBG(cout << "TokenDO::compile() calling statement->compile(pgm, regdp)" << endl);
    statement->compile(pgm, regdp); 	// execute loop's statement(s)
    Operand &reg = condition->compile(pgm, regdp); // get condition result
    DBG(pgm.cc.comment("TokenDO::compile() pgm.safetest(reg, reg)"));
    pgm.testzero(reg);			// compare to zero
    pgm.cc.je(dotail);			// jump to end

    pgm.cc.bind(dodo);			// bind action label
    pgm.cc.jmp(dotop);			// jump back to top
    pgm.cc.bind(dotail);		// bind do tail

    pgm.loopstack.pop();		// pop labels from loopstack
    DBG(std::cout << "TokenDO::compile() END" << std::endl);

    return reg;
}

// while ( condition ) statement;
// TODO: need way to support break and continue
Operand &TokenWHILE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenWHILE::compile() TOP" << std::endl);
    Label whiletop  = pgm.cc.newLabel();	// label for top of loop
    Label whiledo   = pgm.cc.newLabel();	// label for loop action
    Label whiletail = pgm.cc.newLabel();	// label for tail of loop

    pgm.loopstack.push(make_pair(&whiletop, &whiletail)); // push labels onto loopstack
    pgm.cc.bind(whiletop);			// label the top of the loop
    DBG(pgm.cc.comment("condition->compile(pgm, regdp)"));
    Operand &reg = condition->compile(pgm, regdp);// get condition result
    DBG(pgm.cc.comment("TokenWHILE::compile() pgm.safetest(reg, reg)"));
    pgm.testzero(reg);  //    pgm.safetest(reg, reg);			// compare to zero
    pgm.cc.je(whiletail);			// if zero, jump to end

    DBG(cout << "TokenWHILE::compile() calling statement->compile(pgm, regdp)" << endl);
    pgm.cc.bind(whiledo);			// bind action label
    DBG(pgm.cc.comment("statement->compile(pgm, regdp)"));
    statement->compile(pgm, regdp); 		// execute loop's statement(s)
    pgm.cc.jmp(whiletop);			// jump back to top
    pgm.cc.bind(whiletail);			// bind while tail

    pgm.loopstack.pop();			// pop labels from loopstack
    DBG(std::cout << "TokenWHILE::compile() END" << std::endl);

    return reg;
}

Operand &TokenFOR::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenFOR::compile() TOP" << std::endl);
    Label fortop  = pgm.cc.newLabel();		// label for top of loop
    Label forcont = pgm.cc.newLabel();		// label for continue statement
    Label fortail = pgm.cc.newLabel();		// label for tail of loop

    pgm.loopstack.push(make_pair(&forcont, &fortail)); // push labels onto loopstack
    initialize->compile(pgm, regdp); 		// execute loop's initializer statement
    pgm.cc.bind(fortop);			// label the top of the loop
    Operand &reg = condition->compile(pgm, regdp); // get condition result
    DBG(pgm.cc.comment("TokenFOR::compile() pgm.safetest(reg, reg)"));
    pgm.testzero(reg);				// compare to zero
    pgm.cc.je(fortail);				// jump to end

    DBG(cout << "TokenFOR::compile() calling statement->compile(pgm, regdp)" << endl);
    statement->compile(pgm, regdp); 		// execute loop's statement(s)
    pgm.cc.bind(forcont);			// bind continue label
    increment->compile(pgm, regdp); 		// execute loop's increment statement
    pgm.cc.jmp(fortop);				// jump back to top
    pgm.cc.bind(fortail);			// bind for tail

    pgm.loopstack.pop();			// pop labels from loopstack
    DBG(std::cout << "TokenFOR::compile() END" << std::endl);

    return reg;
}
