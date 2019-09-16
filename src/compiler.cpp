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
}

void streamout_string(std::ostream &os, std::string &s)
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
//  DBG(std::cout << "streamout_int: << " << i << std::endl);
    os << i;
}

void streamout_intptr(std::ostream &os, int *i)
{
    if ( !i ) { std::cerr << "ERROR: streamout_intptr: NULL!" << std::endl; return; }
    DBG(std::cout << "streamout_intptr: << " << *i << std::endl);
    os << *i;
}

// simple for now, should have different versions for signed vs unsigned
// small to big vs big to small, etc, as we need to ensure that moving
// small to big doesn't leave unwanted data in the other part of the register
void Program::safemov(x86::Gp &r1, x86::Gp &r2)
{
    switch(r1.type())
    {
	case BaseReg::kTypeGp8Lo: cc.mov(r1, r2.r8Lo());  break;
	case BaseReg::kTypeGp8Hi: cc.mov(r1, r2.r8Hi());  break;
	case BaseReg::kTypeGp16:  cc.mov(r1, r2.r16());   break;
	case BaseReg::kTypeGp32:  cc.mov(r1, r2.r32());   break;
	case BaseReg::kTypeGp64:  cc.mov(r1, r2.r64());   break;
	default: throw "Program::safemov() cannot match register types";
    }
}

// compare two registers even if they are different sizes
void Program::safecmp(x86::Gp &lval, x86::Gp &rval)
{
    if ( lval.size() != rval.size() )
	cc.cmp(lval.r64(), rval.r64());
    else
	cc.cmp(lval, rval);
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
}

bool Program::_compiler_finalize()
{
    cc.ret(); // extra ret just in case
    if ( !cc.finalize() )
    {
	std::cerr << "Finalize failed!" << std::endl;
	return false;
    }
    jit.add(&root_fn, &code);
    if ( !root_fn )
    {
	std::cerr << "Code generation failed!" << std::endl;
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

x86::Gp& TokenCallFunc::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCallFunc::compile(" << var.name << ") TOP" << endl);

    if ( !var.type->is_function() )
	throw "TokenCallFunc::compile() called on non-function";

    Method *method;
    FuncNode *fnd;

    // grab the Method object
    if ( !(method=(Method *)var.data) )
	throw "TokenCallFunc::compile() function method is NULL";

    // grab the FuncNode object
    if ( !(fnd=((FuncDef *)(method->returns.type))->funcnode) && !method->x86code )
	throw "TokenCallFunc::compile() method has neither FuncNode nor x86code";
#if 0
    // simplify if no arguments
    if ( !argc() )
    {
	pgm.cc.call(fnd->label(), FuncSignatureT<void, void>(CallConv::kIdHost));
	return;
    }
#endif

    // build arguments
    FuncDef *func = (FuncDef *)method->returns.type;
    FuncSignatureBuilder funcsig(CallConv::kIdHost);
    std::vector<x86::Gp> params;
    DataDef *ptype;
    uint32_t _argc;
    TokenBase *tn;
    TokenVar *tv;

    // set return type
    switch(func->returns.type())
    {
	case DataType::dtSTRING:	funcsig.setRetT<void *>();		break;
	case DataType::dtCHAR:		funcsig.setRetT<char>();		break;
	case DataType::dtBOOL:		funcsig.setRetT<bool>();		break;
//	case DataType::dtINT:		funcsig.setRetT<int>();			break;
	case DataType::dtINT16:		funcsig.setRetT<int16_t>();		break;
	case DataType::dtINT24:		funcsig.setRetT<int16_t>();		break;
	case DataType::dtINT32:		funcsig.setRetT<int32_t>();		break;
	case DataType::dtINT64:		funcsig.setRetT<int64_t>();		break;
	case DataType::dtUINT8:		funcsig.setRetT<uint8_t>();		break;
	case DataType::dtUINT16:	funcsig.setRetT<uint16_t>();		break;
	case DataType::dtUINT24:	funcsig.setRetT<uint16_t>();		break;
	case DataType::dtUINT32:	funcsig.setRetT<uint32_t>();		break;
	case DataType::dtUINT64:	funcsig.setRetT<uint64_t>();		break;
	case DataType::dtCHARptr:	funcsig.setRetT<const char *>();	break;
	case DataType::dtVOID:		funcsig.setRetT<void>();		break;
	default:			funcsig.setRetT<void *>();		break;
    }
    if ( !regdp.first )
    {
	DBG(pgm.cc.comment("TokenCallFunc::compile() getreg() to assign _reg"));
	getreg(pgm); // assign _reg if not provided
	regdp.first = &_reg;
    }

#if OBJECT_SUPPORT
    // pass along object ("this") as first argument if appropriate
    if ( obj && func->parameters.size() )
    {
	ptype = func->parameters[0];
	if ( ptype->is_object() )
	{
	    funcsig.addArgT<void *>();
	    params.push_back(obj->getreg(pgm)); // params.push_back(tkFunction->getreg(pgm.cc, obj));
	}
	else
	    DBG(std::cout << "TokenCallFunc::compile() got obj param, but param[0] is not an object: " << (int)ptype->type() << std::endl);
    }
#endif

    if ( argc() > func->parameters.size() )
    {
	std::cerr << "ERROR: TokenCallFunc::compile() method " << var.name << " called with too many parameters" << std::endl;
	std::cerr << "argc(): " << argc() << " func->parameters.size(): " << func->parameters.size() << std::endl;
	for ( int i = 0; i < argc(); ++i )
	{
	    tn = parameters[i];
	    std::cerr << "arg[" << i << "] type() = " << (int)tn->type() << " id() = " << (int)tn->id() << std::endl;
	}
	throw "TokenCallFunc::compile() called with too many parameters";
    }

    for ( int i = 0; i < argc(); ++i )
    {
	ptype = func->parameters[i];
	tn = parameters[i];

	switch(tn->type())
	{
	    case TokenType::ttVariable:
		tv = dynamic_cast<TokenVar *>(tn);
		// TODO: replace all this with a ptype->is_compatible(tv->var.type)
		if ( ptype->is_numeric() && !tv->var.type->is_numeric() )
		{
		    DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)tv->var.type->type() << endl);
		    throw "Expecting numeric argument";
		}
		if ( ptype->is_string() && !tv->var.type->is_string() )
		    throw "Expecting string argument";
		if ( ptype->is_object() )
		{
		    if ( !tv->var.type->is_object() )
			throw "Expecting object argument";
		    // check for has_ostream / has_istream
		    if ( ptype->rawtype() != tv->var.type->rawtype() )
			throw "Object type mismatch";
		}
		DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(tv->getreg(pgm))"));
		params.push_back(tv->getreg(pgm)); // params.push_back(pgm.tkFunction->getreg(pgm.cc, &tv->var));
		// could probably use a tv->var.addArgT(funcsig) method
		switch(tv->var.type->type())
		{
		    case DataType::dtCHAR:	funcsig.addArgT<char>();	break;
		    case DataType::dtBOOL:	funcsig.addArgT<bool>();	break;
//		    case DataType::dtINT:	funcsig.addArgT<int>();		break;
//		    case DataType::dtINT8:	funcsig.addArgT<int8_t>();	break;
		    case DataType::dtINT16:	funcsig.addArgT<int16_t>();	break;
		    case DataType::dtINT24:	funcsig.addArgT<int16_t>();	break;
		    case DataType::dtINT32:	funcsig.addArgT<int32_t>();	break;
		    case DataType::dtINT64:	funcsig.addArgT<int64_t>();	break;
		    case DataType::dtUINT8:	funcsig.addArgT<uint8_t>();	break;
		    case DataType::dtUINT16:	funcsig.addArgT<uint16_t>();	break;
		    case DataType::dtUINT24:	funcsig.addArgT<uint16_t>();	break;
		    case DataType::dtUINT32:	funcsig.addArgT<uint32_t>();	break;
		    case DataType::dtUINT64:	funcsig.addArgT<uint64_t>();	break;
		    case DataType::dtCHARptr:	funcsig.addArgT<const char *>();break;
		    default:			funcsig.addArgT<void *>();	break;
		} // switch
		break;
	    case TokenType::ttInteger: // integer literal
		if ( ptype->type() > DataType::dtRESERVED )
		{
		    std::cerr << "ERROR: TokenCallFunc::compile() method " << var.name << " argument " << i << " was expecting type: " << ptype->name << " not integer" << std::endl;
		    throw "Not expecting numeric argument";
		}
		DBG(std::cout << "TokenCallFunc::compile() adding call to (ttInteger): " << method->returns.name << '(' << tn->val() << ')' << std::endl);
		{
		    x86::Gp p = pgm.cc.newGpq();
		    pgm.cc.mov(p, tn->val());
		    params.push_back(p);
		}
		funcsig.addArgT<int>();
		break;
	    case TokenType::ttChar: // literal char
		DBG(std::cout << "TokenCallFunc::compile() adding call to (ttChar): " << method->returns.name << '(' << tn->val() << ')' << std::endl);
		{
		    x86::Gp p = pgm.cc.newGpb();
		    pgm.cc.mov(p, tn->val());
		    params.push_back(p);
		}
		funcsig.addArgT<char>();
		break;
	    case TokenType::ttCallFunc:
		DBG(std::cout << "TokenCallFunc::compile() adding call to (ttCallFunc): " << method->returns.name << '(' << tn->val() << ')' << std::endl);
		{
		    x86::Gp &p = tn->compile(pgm, regdp);
		    params.push_back(p);
		}
		funcsig.addArgT<int>();
		break;
	    default:
		std::cerr << "TokenCallFunc::compile() parameter #" << i << " is type " << (int)tn->type() << std::endl;
		throw "Unsupported type";
	} // switch
    }

    if ( !fnd )
	DBG(std::cout << "TokenCallFunc::compile(cc.call(" << (uint64_t)method->x86code << ')' << std::endl);

    // now we should have all we need to call the function
    DBG(pgm.cc.comment(var.name.c_str()));
    FuncCallNode *call = fnd ? pgm.cc.call(fnd->label(), funcsig) : pgm.cc.call(imm(method->x86code), funcsig);
    std::vector<x86::Gp>::iterator gvi;
    _argc = 0;

    for ( gvi = params.begin(); gvi != params.end(); ++gvi )
    {
	DBG(std::cout << "TokenCallFunc::compile(call->setArg(" << _argc << ", reg)" << endl);
	call->setArg(_argc++, *gvi);
    }

    DBG(std::cout << "TokenCallFunc::compile() END" << std::endl);
#if 1
    // handle return value
    if ( regdp.first )
    {
	call->setRet(0, *regdp.first);
	return *regdp.first;
    }
    else
#endif
    if ( func->returns.type() != DataType::dtVOID )
    {
	call->setRet(0, _reg);
    }

    return _reg;
}

x86::Gp& TokenCpnd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") TOP" << endl);
    x86::Gp *regp = &_reg;
    for ( vector<TokenStmt *>::iterator vti = statements.begin(); vti != statements.end(); ++vti )
    {
	regp = &(*vti)->compile(pgm, regdp);
    }
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") END" << endl);

    return *regp;
}

// compile the "program" token, which contains all initilization / non-function statements
x86::Gp& TokenProgram::compile(Program &pgm, regdefp_t &regdp)
{
    if ( this != pgm.tkProgram ) { throw "this != tkProgram"; }
    DBG(cout << "TokenProgram::compile(" << (uint64_t)this << ") TOP" << endl);
    DBG(cout << "    source: " << source << endl);
    DBG(cout << "     bytes: " << bytes << endl);
    DBG(cout << "     lines: " << lines << endl);

    pgm.tkFunction = pgm.tkProgram;
    pgm.tkFunction->clear_regmap();	// clear register map

    pgm.cc.addFunc(FuncSignatureT<void, void>(CallConv::kIdHost));

    for ( vector<TokenStmt *>::iterator si = statements.begin(); si != statements.end(); ++si )
    {
	/*_reg =*/ (*si)->compile(pgm, regdp);
    }

    pgm.tkFunction->cleanup(pgm.cc);	// cleanup stack
    pgm.cc.ret();			// always add return in case source doesn't have one
    pgm.cc.endFunc();		// end function

    pgm.tkFunction->clear_regmap(); // clear register map

    DBG(cout << "TokenProgram::compile(" << (uint64_t)this << ") END" << endl);

    return _reg;
}

x86::Gp& TokenBase::compile(Program &pgm, regdefp_t &regdp)
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
	    DBG(cout << "TokenStmt::compile() TokenInt(" << val() << ')' << endl);
	    return dynamic_cast<TokenInt *>(this)->compile(pgm, regdp);
	case TokenType::ttVariable:
	    DBG(cout << "TokenStmt::compile() TokenVar(" << dynamic_cast<TokenVar *>(this)->var.name << ')' << endl);
	    return dynamic_cast<TokenVar *>(this)->compile(pgm, regdp);
	case TokenType::ttCallFunc:
	    return dynamic_cast<TokenCallFunc *>(this)->compile(pgm, regdp);
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

x86::Gp& TokenDecl::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDecl::compile(" << var.name << ") TOP" << endl);

    if ( initialize )
	initialize->compile(pgm, regdp);

    DBG(cout << "TokenDecl::compile(" << var.name << ") END" << endl);

    return _reg;
}

x86::Gp& TokenFunc::compile(Program &pgm, regdefp_t &regdp)
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
    clear_regmap();	// clear register map

    pgm.cc.addFunc(func->funcnode);

    if ( method.parameters.size() )
    {
	DBG(cout << "TokenFunc::compile() has parameters:" << endl);
	uint32_t argc = 0;

	for ( variable_vec_iter vvi = method.parameters.begin(); vvi != method.parameters.end(); ++vvi )
	{
	    DBG(std::cout << "TokenFunc::compile(): cc.setArg(" << argc << ", " << (*vvi)->name << ')' << std::endl);
	    x86::Gp &reg = getvreg(pgm.cc, (*vvi));
	    pgm.cc.setArg(argc++, reg);
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
	(*si)->compile(pgm, regdp);
    }

    cleanup(pgm.cc);	// cleanup stack
    pgm.cc.ret();	// always add return in case source doesn't have one
    pgm.cc.endFunc();	// end function

    clear_regmap();	// clear register map

    DBG(cout << "TokenFunc::compile(" << var.name << ") END" << endl);

    return _reg;
}


// compile the code tree into x86 code
bool Program::compile()
{
    TokenBase *tb;
    regdefp_t regdp;

    DBG(cout << endl << endl << "Program::compile() start" << endl << endl);
    _compiler_init();

    try
    {
	while ( !ast.empty() )
	{
	    tb = ast.front();
	    DBG(cout << "Program::compile(" << (void *)tb << ')' << endl);
	    ast.pop();
	    tb->compile(*this, regdp);
	}
    }
    catch(const char *err_msg)
    {
	if ( tb )
	    cerr << ANSI_WHITE;
	else
	    cerr << ANSI_WHITE << (tb->file ? tb->file : "NULL") << ':' << tb->line << ':' << tb->column;
	cerr << ": \e[1;31merror:\e[1;37m " << err_msg << ANSI_RESET << endl;
	return false;
    }
    catch(TokenBase *tb)
    {
	cerr << ANSI_WHITE << (tb->file ? tb->file : "NULL") << ':' << tb->line << ':' << tb->column
	     << ": \e[1;31merror:\e[1;37m unexpected token type " << (int)tb->type() << " value " << (int)tb->get() << " char " << (char)tb->get() << ANSI_RESET << endl;
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
    DBG(std::cout << "Program::execute() calling main()" << std::endl << std::endl);
    main_fn();
    DBG(std::cout << std::endl << "Program::execute() main() returns" << std::endl);
    DBG(std::cout << "Program::execute() ends" << std::endl);    
}

// compile the increment operator
x86::Gp& TokenInc::compile(Program &pgm, regdefp_t &regdp)
{
    TokenVar *tv;

    // left has precedence over right
    if ( left )
    {
	if ( left->type() != TokenType::ttVariable )
	    throw "Increment on a non-variable lval";
	tv = dynamic_cast<TokenVar *>(left);
	x86::Gp &reg = tv->getreg(pgm);
	pgm.cc.inc(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    if ( right )
    {
	if ( right->type() != TokenType::ttVariable )
	    throw "Increment on a non-variable rval";
	tv = dynamic_cast<TokenVar *>(right);
	x86::Gp &reg = tv->getreg(pgm);
	pgm.cc.inc(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    throw "Invalid increment";
}

// compile the decrement operator
x86::Gp& TokenDec::compile(Program &pgm, regdefp_t &regdp)
{
    TokenVar *tv;

    // left has precedence over right
    if ( left )
    {
	if ( left->type() != TokenType::ttVariable )
	    throw "Decrement on a non-variable lval";
	tv = dynamic_cast<TokenVar *>(left);
	x86::Gp &reg = tv->getreg(pgm);
	pgm.cc.dec(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    if ( right )
    {
	if ( right->type() != TokenType::ttVariable )
	    throw "Decrement on a non-variable rval";
	tv = dynamic_cast<TokenVar *>(right);
	x86::Gp &reg = tv->getreg(pgm);
	pgm.cc.dec(reg);
	tv->var.modified();
	tv->putreg(pgm);
	return reg;
    }
    throw "Invalid increment";
}

// assignment left = right
x86::Gp& TokenAssign::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAssign::compile() TOP" << endl);
    TokenVar *tvl, *tvr;
    TokenCallFunc *tcr;
    TokenIdent *tidn;
    TokenDot *tdot = NULL;
    x86::Gp *regp;
    DataDef *ltype;

    if ( !left )
	throw "Assignment with no lval";
    if ( !right )
	throw "Assignment with no rval";

    if ( left->type() == TokenType::ttVariable )
    {
	tvl = dynamic_cast<TokenVar *>(left);
	ltype = tvl->var.type;
	DBG(cout << "TokenAssign::compile() assignment to " << tvl->var.name << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() assignment to:"));
	DBG(pgm.cc.comment(tvl->var.name.c_str()));
	DBG(pgm.cc.comment("TokenAssign::compile() regp = tvl->getreg(pgm)"));
	regp = &tvl->getreg(pgm);
    }
    else
    if ( left->id() == TokenID::tkDot && ((TokenDot *)left)->left
    &&  ((TokenDot *)left)->left->type() == TokenType::ttVariable
    &&  ((TokenDot *)left)->right && ((TokenDot *)left)->right->type() == TokenType::ttIdentifier )
    {
	tdot = (TokenDot *)left;
	tidn = ((TokenIdent *)tdot->right);
	tvl = dynamic_cast<TokenVar *>(tdot->left);
	if ( !tvl->var.type->is_struct() && !tvl->var.type->is_object() )
	    throw "Expecting class or structure";
	if ( !(ltype=((DataDefSTRUCT *)tvl->var.type)->m_type(tidn->str)) )
	    throw "Unidentifier member";
	ssize_t ofs = ((DataDefSTRUCT *)tvl->var.type)->m_offset(tidn->str);
	x86::Gp &lval = tvl->getreg(pgm);
	x86::Mem m = x86::ptr(lval, ofs);
	// _reg points to our data for this member
	_reg = pgm.cc.newIntPtr(tvl->var.name.c_str());
	pgm.cc.lea(_reg, m);
	regp = &_reg;
//	regp = &tdot->compile(pgm, regdp);
    }
    else
    {
	throw "Assignment on a non-variable lval";
    }

    // get left register
    x86::Gp &lreg = *regp;

    switch(right->type())
    {
	case TokenType::ttVariable:
	    tvr = dynamic_cast<TokenVar *>(right);
	    DBG(cout << "TokenAssign::compile() assignment from " << tvr->var.name << endl);
	    if ( &tvl->var == &tvr->var && !tdot )
	    {
		DBG(cout << "TokenAssign::compile() assigning variable to itself? nothing to do!" << endl);
		break;
	    }
	    if ( ltype->is_numeric() && tvr->var.type->is_numeric() )
	    {
		DBG(cout << "TokenAssign::compile() numeric to numeric" << endl);
		DBG(pgm.cc.comment("TokenAssign::compile() rreg = tvr->getreg(pgm)"));
		x86::Gp &rreg = tvr->getreg(pgm);
		DBG(pgm.cc.comment("TokenAssign::compile() numeric to numeric"));
		DBG(pgm.cc.comment("TokenAssign::compile() pgm.safemov(lreg, rreg)"));
		pgm.safemov(lreg, rreg);
		tvl->var.modified();
		DBG(pgm.cc.comment("TokenAssign::compile() pgm.putreg(pgm)"));
		tvl->putreg(pgm);
		break;
	    }
	    if ( ltype->is_string() && tvr->var.type->is_string() )
	    {
		DBG(cout << "TokenAssign::compile() string to string" << endl);
		DBG(cout << "TokenAssign::compile() will call " << tvl->var.name << '('
		    << (tvl->var.data ? ((string *)(tvl->var.data))->c_str() : "") << ").assign[" << (uint64_t)string_assign << "](" << tvr->var.name
		    << '(' << (tvr->var.data ? ((string *)(tvr->var.data))->c_str() : "") << ')' << endl);

		x86::Gp &rreg = tvr->getreg(pgm);
		DBG(pgm.cc.comment("string_assign"));
		FuncCallNode* call = pgm.cc.call(imm(string_assign), FuncSignatureT<void, const char*, const char *>(CallConv::kIdHost));
		call->setArg(0, lreg);
		call->setArg(1, rreg);
		tvl->var.modified();
		tvl->putreg(pgm);
		break;
	    }
	    if ( ltype->type() != tvr->var.type->type() )
		throw "Variable types do not match";
	    else
		throw "Unimplemented assignment";
	case TokenType::ttInteger:
	    if ( ltype->is_numeric() )
	    {
		DBG(cout << "TokenAssign::compile() numeric pgm.cc.mov(" << tvl->var.name << ".reg, " << right->val() << ')' << endl);
		DBG(pgm.cc.comment("TokenAssign::compile() numeric pgm.cc.mov(lreg, right->val)"));
		if ( tdot )
		    ltype->movint2rptr(pgm.cc, lreg, right->val());
//		    pgm.cc.mov(qword_ptr(lreg), right->val());
		else
		    pgm.cc.mov(lreg, right->val());
		tvl->var.modified();
		tvl->putreg(pgm);
		break;
	    }
	    throw "Variable non-numeric";
	case TokenType::ttCallFunc:
	    tcr = dynamic_cast<TokenCallFunc *>(right);
	    DBG(cout << "TokenAssign::compile() TokenCallFunc(" << tcr->var.name << ')' << endl);
	    if ( (ltype->is_numeric() && tcr->returns()->is_numeric())
	    ||   (ltype == tcr->returns()) )
	    {
		DBG(pgm.cc.comment("TokenAssign::compile() calling tcr->compile(pgm, ret{lreg})"));
		regdp.first = &lreg;
		tcr->compile(pgm, regdp);
		tvl->var.modified();
		DBG(pgm.cc.comment("TokenAssign::compile() calling tvl->putreg(pgm)"));
		tvl->putreg(pgm);
		break;
	    }
	    cerr << "TokenAssign::compile() ltype->type(): " << (int)ltype->type() << " tcr->returns(): " << (int)tcr->returns()->type() << endl;
	    throw "Return type not compatible";
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	    {
		// temp for right side
//		x86::Gp rreg = pgm.cc.newGpq();
//		right->compile(pgm, &rreg);
//		pgm.safemov(lreg, rreg);
		x86::Gp &rreg = right->compile(pgm, regdp);
		pgm.safemov(lreg, rreg);
		break;
	    }
	default:
	    throw "Unimplemented assignment";
    } // switch

    DBG(cout << "TokenAssign::compile() END" << endl);

    return lreg;
}

// base defaults to 8bit register
x86::Gp &TokenBase::getreg(Program &pgm)
{
    _reg = pgm.cc.newGpb();
    pgm.cc.mov(_reg, _token);
    return _reg;
}

// operator gets 64bit set to 0
x86::Gp &TokenOperator::getreg(Program &pgm)
{
    _reg = pgm.cc.newGpq();
    pgm.cc.xor_(_reg, _reg);
    return _reg;
}

// integer gets 64bit
x86::Gp &TokenInt::getreg(Program &pgm)
{
    if ( _reg.type() != BaseReg::kTypeGp64 )
    {
	DBG(cout << "TokenInt::getreg() _reg.type() = " << (int)_reg.type() << " != " << (int)BaseReg::kTypeGp64 << endl);
	DBG(pgm.cc.comment("TokenInt::getreg() type() = 0, _reg = newGpq()"));
	_reg = pgm.cc.newGpq();
    }
    DBG(pgm.cc.comment("TokenInt::getreg() mov(_reg, _token)"));
    pgm.cc.mov(_reg, _token);
    return _reg;
}

// variable needs special handling
x86::Gp &TokenVar::getreg(Program &pgm)
{
    DBG(pgm.cc.comment("TokenVar::getreg()"));
    return pgm.tkFunction->getvreg(pgm.cc, &var);
}

// variable also needs to be able to write the register back to variable
void TokenVar::putreg(Program &pgm)
{
    pgm.tkFunction->putreg(pgm.cc, &var);
}

// function needs similar handling to variable
x86::Gp &TokenCallFunc::getreg(Program &pgm)
{
    _reg = returns()->newreg(pgm.cc, var.name.c_str());
#if 0
    switch(returns()->type())
    {
	case DataType::dtCHAR:    _reg = pgm.cc.newGpb(var.name.c_str());    break;
	case DataType::dtBOOL:    _reg = pgm.cc.newGpb(var.name.c_str());    break;
	case DataType::dtINT64:   _reg = pgm.cc.newGpq(var.name.c_str());    break;
	case DataType::dtINT16:   _reg = pgm.cc.newGpw(var.name.c_str());    break;
	case DataType::dtINT24:   _reg = pgm.cc.newGpw(var.name.c_str());    break;
	case DataType::dtINT32:   _reg = pgm.cc.newGpd(var.name.c_str());    break;
	case DataType::dtUINT8:   _reg = pgm.cc.newGpb(var.name.c_str());    break;
	case DataType::dtUINT16:  _reg = pgm.cc.newGpw(var.name.c_str());    break;
	case DataType::dtUINT24:  _reg = pgm.cc.newGpw(var.name.c_str());    break;
	case DataType::dtUINT32:  _reg = pgm.cc.newGpd(var.name.c_str());    break;
	case DataType::dtUINT64:  _reg = pgm.cc.newGpq(var.name.c_str());    break;
	default:		  _reg = pgm.cc.newIntPtr(var.name.c_str()); break;
    } // switch
//  compile(pgm, &_reg);
#endif
    return _reg;
}

void TokenCpnd::movreg(x86::Compiler &cc, x86::Gp &reg, Variable *var)
{
    DBG(cc.comment("TokenCpnd::movreg() calling movmptr2rval(cc, reg, var->data)"));
    DBG(cc.comment(var->name.c_str()));
    var->type->movmptr2rval(cc, reg, var->data);
/*
    switch(var->type->type())
    {
	case DataType::dtCHAR:    cc.mov(reg, asmjit::x86::byte_ptr((uintptr_t)var->data));  break;
	case DataType::dtBOOL:    cc.mov(reg, asmjit::x86::byte_ptr((uintptr_t)var->data));  break;
	case DataType::dtINT64:   cc.mov(reg, asmjit::x86::qword_ptr((uintptr_t)var->data)); break;
	case DataType::dtINT16:   cc.mov(reg, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
	case DataType::dtINT24:   cc.mov(reg, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
	case DataType::dtINT32:   cc.mov(reg, asmjit::x86::dword_ptr((uintptr_t)var->data)); break;
	case DataType::dtUINT8:   cc.mov(reg, asmjit::x86::byte_ptr((uintptr_t)var->data));  break;
	case DataType::dtUINT16:  cc.mov(reg, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
	case DataType::dtUINT24:  cc.mov(reg, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
	case DataType::dtUINT32:  cc.mov(reg, asmjit::x86::dword_ptr((uintptr_t)var->data)); break;
	case DataType::dtUINT64:  cc.mov(reg, asmjit::x86::qword_ptr((uintptr_t)var->data)); break;
	default:		  cc.mov(reg, asmjit::imm(var->data));			     break;
    } // switch
*/
}

// Manage registers for use on local as well as global variables
x86::Gp &TokenCpnd::getvreg(x86::Compiler &cc, Variable *var)
{
    std::map<Variable *, x86::Gp>::iterator rmi;

    DBG(cc.comment("TokenCpnd::getreg() on"));
    DBG(cc.comment(var->name.c_str()));

    if ( (rmi=register_map.find(var)) != register_map.end() )
    {
	DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::getreg(" << var->name << ") found" << std::endl);
	// copy global variable to register -- needs to happen every time we need to access a global
	if ( var->is_global() && var->data && !var->is_constant() )
	{
	    DBG(cc.comment("TokenCpnd::getreg() variable found, var->is_global() && var->data && !var->is_constant()"));
	    movreg(cc, rmi->second, var);
        }
	return rmi->second;
    }

    DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::getreg(" << var->name << ") building register" << std::endl);
    if ( (var->flags & vfSTACK) && !var->type->is_numeric() )
    {
	switch(var->type->type())
	{
	    case DataType::dtSTRING:
		{
		    x86::Mem stack = cc.newStack(sizeof(std::string), 4);
		    x86::Gp reg = cc.newIntPtr(var->name.c_str());
		    cc.lea(reg, stack);
		    DBG(std::cout << "TokenCpnd::getreg(" << var->name << ") stack var calling string_construct[" << (uint64_t)string_construct << ']' << std::endl);
		    DBG(cc.comment("string_construct"));
		    FuncCallNode *call = cc.call(imm(string_construct), FuncSignatureT<void *, void *>(CallConv::kIdHost));
		    call->setArg(0, reg);
		    register_map[var] = reg;
		}
		break;
	    case DataType::dtSSTREAM:
		{
		    x86::Mem stack = cc.newStack(sizeof(std::stringstream), 4);
		    x86::Gp reg = cc.newIntPtr(var->name.c_str());
		    cc.lea(reg, stack);
		    DBG(cc.comment("stringstream_construct"));
		    FuncCallNode *call = cc.call(imm(stringstream_construct), FuncSignatureT<void *, void *>(CallConv::kIdHost));
		    call->setArg(0, reg);
		    register_map[var] = reg;
		}
		break;
	    case DataType::dtOSTREAM:
		{
		    x86::Mem stack = cc.newStack(sizeof(std::ostream), 4);
		    x86::Gp reg = cc.newIntPtr(var->name.c_str());
		    cc.lea(reg, stack);
		    register_map[var] = reg;
		}
		break;
	    default:
		if ( var->type->reftype() == RefType::rtReference
		||   var->type->reftype() == RefType::rtPointer )
		{
		    x86::Gp reg = cc.newIntPtr(var->name.c_str());
		    register_map[var] = reg;
		    break;
		}
		if ( var->type->basetype() == BaseType::btStruct
		||   var->type->basetype() == BaseType::btClass )
		{
		    x86::Mem stack = cc.newStack(var->type->size, 4);
		    x86::Gp reg = cc.newIntPtr(var->name.c_str());
		    cc.lea(reg, stack);
//		    cc.mov(qword_ptr(reg, sizeof(string)), 0);
		    register_map[var] = reg;
		    break;
		}
		std::cerr << "unsupported type: " << (int)var->type->type() << std::endl;
		std::cerr << "reftype: " << (int)var->type->reftype() << std::endl;
		throw "TokenCpnd()::getreg() unsupported type on stack";
		
	} // switch
    }
    else
    {
	DBG(cc.comment("TokenCpnd::getreg() calling var->type->newreg()"));
	register_map[var] = var->type->newreg(cc, var->name.c_str());
/*
        // assign new register for duration of function
	switch(var->type->type())
	{
	    case DataType::dtCHAR:    register_map[var] = cc.newGpb(var->name.c_str());    break;
	    case DataType::dtBOOL:    register_map[var] = cc.newGpb(var->name.c_str());    break;
	    case DataType::dtINT64:   register_map[var] = cc.newGpq(var->name.c_str());    break;
	    case DataType::dtINT16:   register_map[var] = cc.newGpw(var->name.c_str());	   break;
	    case DataType::dtINT24:   register_map[var] = cc.newGpw(var->name.c_str());	   break;
	    case DataType::dtINT32:   register_map[var] = cc.newGpd(var->name.c_str());	   break;
	    case DataType::dtUINT8:   register_map[var] = cc.newGpb(var->name.c_str());	   break;
	    case DataType::dtUINT16:  register_map[var] = cc.newGpw(var->name.c_str());	   break;
	    case DataType::dtUINT24:  register_map[var] = cc.newGpw(var->name.c_str());	   break;
	    case DataType::dtUINT32:  register_map[var] = cc.newGpd(var->name.c_str());    break;
	    case DataType::dtUINT64:  register_map[var] = cc.newGpq(var->name.c_str());    break;
	    default:		      register_map[var] = cc.newIntPtr(var->name.c_str()); break;
	} // switch
*/
	if ( (rmi=register_map.find(var)) == register_map.end() )
	    throw "TokenCpnd::getreg() failure";
	DBG(cc.comment("TokenCpnd::getreg() variable reg init, calling movreg on"));
	DBG(cc.comment(var->name.c_str()));
	if ( !(var->flags & vfSTACK) )
	    movreg(cc, rmi->second, var); // first initialization of non-stack register (regset)
	else
	if ( !(var->flags & vfPARAM) )
	// if it's a numeric stack register, we set it to zero, for the full size of the register
	// because subsequent operations (assignments, etc), may only access less significant
        // parts depending on the integer size, also, if we don't touch it here, we may not keep
        // access to this specific register for this variable
	if ( var->type->is_numeric() )
	    cc.xor_(rmi->second.r64(), rmi->second.r64());
    }
    var->flags |= vfREGSET;

    if ( rmi == register_map.end() && (rmi=register_map.find(var)) == register_map.end() )
	throw "TokenCpnd::getreg() failure";
    return rmi->second;
}


// only used for global varibles -- move register back into variable data
void TokenCpnd::putreg(asmjit::x86::Compiler &cc, Variable *var)
{
    // shortcut out if we can't work with this variable
    if ( !(var->is_global() && var->data && (var->flags & vfREGSET) && (var->flags & vfMODIFIED) && var->type->is_numeric()) )
	return;

    std::map<Variable *, asmjit::x86::Gp>::iterator rmi;
    if ( (rmi=register_map.find(var)) == register_map.end() )
    {
	std::cerr << "TokenCpnd[" << (uint64_t)this << "]::putreg(" << var->name << ") not found in register_map" << std::endl;
	throw "TokenCpnd::setreg() called on unregistered variable";
    }

    // copy register to global variable -- needs to happen
    // every time we modify a numeric global variable
    DBG(std::cout << "TokenCpnd::putreg[" << (uint64_t)this << "](" << var->name << ") calling cc->mov(data, reg)" << std::endl);
    DBG(cc.comment("TokenCpnd::putreg() calling cc.mov(var->data, reg)"));
    var->type->movrval2mptr(cc, var->data, rmi->second);

    var->flags &= ~vfMODIFIED;
}

// cleanup function: will call destructors on all stack objects
void TokenCpnd::cleanup(asmjit::x86::Compiler &cc)
{
    std::map<Variable *, asmjit::x86::Gp>::iterator rmi;

    DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::cleanup()" << std::endl);

    for ( rmi = register_map.begin(); rmi != register_map.end(); ++rmi )
    {
	if ( (rmi->first->flags & vfSTACK) )
	{
	    if ( rmi->first->type->type() > DataType::dtRESERVED )
	    {
		x86::Gp &reg = rmi->second;
		Variable *var = rmi->first;

		switch(var->type->type())
		{
		    case DataType::dtSTRING:
			{
			    DBG(std::cout << "TokenCpnd::cleanup(" << var->name << ") calling string_destruct[" << (uint64_t)string_destruct << ']' << std::endl);
			    FuncCallNode *call = cc.call(imm(string_destruct), FuncSignatureT<void, void *>(CallConv::kIdHost));
			    call->setArg(0, reg);
			}
			break;
		    case DataType::dtSSTREAM:
			{
			    FuncCallNode *call = cc.call(imm(stringstream_destruct), FuncSignatureT<void, void *>(CallConv::kIdHost));
			    call->setArg(0, reg);
			}
			break;
		    case DataType::dtOSTREAM:
			{
			    FuncCallNode *call = cc.call(imm(ostream_destruct), FuncSignatureT<void, void *>(CallConv::kIdHost));
			    call->setArg(0, reg);
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

// add two integers

x86::Gp& TokenAdd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAdd::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    if ( regdp.first )
    {
	pgm.safemov(*regdp.first, lval);
	DBG(pgm.cc.comment("TokenAdd::compile() pgm.cc.add(*ret, rval)"));
	pgm.cc.add(*regdp.first, rval.r64());

	return *regdp.first;
    }
    _reg = pgm.cc.newGpq();
//  pgm.cc.xor_(_reg, _reg);
    DBG(pgm.cc.comment("TokenAdd::compile() pgm.safemov(_reg, lval)"));
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenAdd::compile() pgm.cc.add(_reg, rval)"));
    pgm.cc.add(_reg, rval.r64());

    return _reg;
}

// subtract two integers
x86::Gp& TokenSub::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenSub::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenSub::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenSub::compile() pgm.cc.sub(_reg, rval)"));
    pgm.cc.sub(_reg, rval.r64());

    return _reg;
}

// make number negative
x86::Gp& TokenNeg::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenNeg::Compile() TOP" << endl);
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenNeg::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, rval);
    DBG(pgm.cc.comment("TokenNeg::compile() pgm.cc.neg(_reg, rval)"));
    pgm.cc.neg(_reg);

    return _reg;
}

// multiply two integers
x86::Gp& TokenMul::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMul::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenMul::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenMul::compile() pgm.cc.imud(_reg, rval)"));
    pgm.cc.imul(_reg, rval.r64());

    return _reg;
}

// divide two integers
x86::Gp& TokenDiv::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDiv::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; } 
    if ( !right ) { throw "!= missing rval operand"; }

    x86::Gp remainder = pgm.cc.newInt64("TokenDiv::remainder");
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    pgm.cc.xor_(remainder, remainder);
    _reg = pgm.cc.newGpq();
    DBG(pgm.cc.comment("TokenDiv::compile() pgm.safemov(_reg, lval)"));
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenDiv::compile() pgm.cc.div(remainder, _reg, rval)"));
    pgm.cc.idiv(remainder, _reg, rval.r64());

    return _reg;
}

// modulus
x86::Gp& TokenMod::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMod::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &remainder = (_reg = pgm.cc.newInt64("remainder"));
    x86::Gp quotent = pgm.cc.newInt64("quotent");
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    pgm.safemov(quotent, lval);
    pgm.cc.xor_(remainder, remainder);
    DBG(pgm.cc.comment("TokenMod::compile() pgm.cc.idiv(remainder, lreg, rval)"));
    pgm.cc.idiv(remainder, quotent, rval.r64());

    return remainder;
}

/////////////////////////////////////////////////////////////////////////////
// bit math operators                                                      //
/////////////////////////////////////////////////////////////////////////////

// bit shift left
x86::Gp& TokenBSL::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBSL::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }

    // hard coding some basic ostream support for now, will use operator overloading later
    if ( left->type() == TokenType::ttVariable && dynamic_cast<TokenVar *>(left)->var.type->has_ostream() )
    {
	TokenCallFunc *tcr;
	TokenVar *tvl = dynamic_cast<TokenVar *>(left);
	DBG(pgm.cc.comment("TokenBSL::compile() (ostream &)tvl->getreg(pgm)"));
	x86::Gp &lval = tvl->getreg(pgm); // get ostream register
	TokenVar *tvr;

	DBG(cout << "TokenBSL::compile() lval(" << tvl->var.name << ")->has_ostream()" << endl);

	switch(right->type())
	{
	    case TokenType::ttVariable:
		DBG(cout << "TokenBSL::compile() right->type() == ttVariable" << endl);
		{
		    tvr = dynamic_cast<TokenVar *>(right);
		    if ( tvr->var.type->is_numeric() )
		    {
			x86::Gp &rval = tvr->getreg(pgm);
			DBG(cout << "TokenBSL::compile() right->var.type->is_numeric()" << endl);
			DBG(pgm.cc.comment("pgm.cc.call(streamout_int)"));
			FuncCallNode *call;
                        switch(tvr->var.type->type())
                        {
			    case DataType::dtCHAR:	call = pgm.cc.call(imm(streamout_numeric<char>), FuncSignatureT<void, void *, char>(CallConv::kIdHost));		break;
			    case DataType::dtBOOL:	call = pgm.cc.call(imm(streamout_numeric<bool>), FuncSignatureT<void, void *, bool>(CallConv::kIdHost));		break;
			    case DataType::dtINT16:	call = pgm.cc.call(imm(streamout_numeric<int16_t>), FuncSignatureT<void, void *, int16_t>(CallConv::kIdHost));		break;
			    case DataType::dtINT24:	call = pgm.cc.call(imm(streamout_numeric<int16_t>), FuncSignatureT<void, void *, int16_t>(CallConv::kIdHost));		break;
			    case DataType::dtINT32:	call = pgm.cc.call(imm(streamout_numeric<int32_t>), FuncSignatureT<void, void *, int32_t>(CallConv::kIdHost));		break;
			    case DataType::dtINT64:	call = pgm.cc.call(imm(streamout_numeric<int64_t>), FuncSignatureT<void, void *, int64_t>(CallConv::kIdHost));		break;
			    case DataType::dtUINT8:	call = pgm.cc.call(imm(streamout_numeric<uint8_t>), FuncSignatureT<void, void *, uint8_t>(CallConv::kIdHost));		break;
			    case DataType::dtUINT16:	call = pgm.cc.call(imm(streamout_numeric<uint16_t>), FuncSignatureT<void, void *, uint16_t>(CallConv::kIdHost));	break;
			    case DataType::dtUINT24:	call = pgm.cc.call(imm(streamout_numeric<uint16_t>), FuncSignatureT<void, void *, uint16_t>(CallConv::kIdHost));	break;
			    case DataType::dtUINT32:	call = pgm.cc.call(imm(streamout_numeric<uint32_t>), FuncSignatureT<void, void *, uint32_t>(CallConv::kIdHost));	break;
			    case DataType::dtUINT64:	call = pgm.cc.call(imm(streamout_numeric<uint64_t>), FuncSignatureT<void, void *, uint64_t>(CallConv::kIdHost));	break;
			    default: throw "TokenBSL::compile() unsupported numeric type";
                        }
			// FuncCallNode* call = pgm.cc.call(imm(streamout_int), FuncSignatureT<void, void *, int>(CallConv::kIdHost));
			call->setArg(0, lval);
			call->setArg(1, rval);
			break;
		    }
		    if ( tvr->var.type->is_string() )
		    {
			x86::Gp &rval = tvr->getreg(pgm);
			DBG(cout << "TokenBSL::compile() right->var.type->is_string()" << endl);
			DBG(pgm.cc.comment("TokenBSL::compile() right->var.type->is_string()"));
			DBG(pgm.cc.comment("pgm.cc.call(streamout_string)"));
			FuncCallNode* call = pgm.cc.call(imm(streamout_string), FuncSignatureT<void, void *, void *>(CallConv::kIdHost));
			DBG(pgm.cc.comment("call->setArg(0, lval)"));
			call->setArg(0, lval);
			DBG(pgm.cc.comment("call->setArg(1, tvr->getreg(pgm))"));
			call->setArg(1, rval); // call->setArg(1, tvr->getreg(pgm));
			break;
		    }
		}
		break;
	    case TokenType::ttInteger:
		DBG(cout << "TokenBSL::compile() right->type() == ttInteger" << endl);
		{
		    x86::Gp &rval = dynamic_cast<TokenInt *>(right)->getreg(pgm);
		    DBG(pgm.cc.comment("pgm.cc.call(streamout_int)"));
		    FuncCallNode* call = pgm.cc.call(imm(streamout_int), FuncSignatureT<void, void *, int>(CallConv::kIdHost));
		    call->setArg(0, lval);
		    call->setArg(1, rval);
		}
		break;
	    case TokenType::ttCallFunc:
		DBG(cout << "TokenBSL::compile() right->type() == ttCallFunc" << endl);
		{
		    tcr = dynamic_cast<TokenCallFunc *>(right);
		    if ( tcr->returns()->has_ostream() ) // i.e. endl
		    {
			// TODO: should use tcr->compile() with object param
			DBG(pgm.cc.comment("pgm.cc.call(tcr->method->x86code) [endl?]"));
			FuncCallNode* call = pgm.cc.call(imm( ((Method *)tcr->var.data)->x86code ), FuncSignatureT<void, void *>(CallConv::kIdHost));
			DBG(pgm.cc.comment("call->setArg(0, lval)"));
			call->setArg(0, lval);
			break;
		    }
		    if ( tcr->returns()->is_numeric() )
		    {
			DBG(cout << "TokenBSL::compile() tcr->returns()->is_numeric()" << endl);
			_reg = pgm.cc.newGpq();
			regdp.first = &_reg;
			tcr->compile(pgm, regdp);
			DBG(pgm.cc.comment("pgm.cc.call(streamout_int)"));
			FuncCallNode* call = pgm.cc.call(imm(streamout_int), FuncSignatureT<void, void *, int>(CallConv::kIdHost));
			call->setArg(0, lval);
			call->setArg(1, _reg);
			break;
		    }
		    break;
		}
	    case TokenType::ttMultiOp:
		DBG(cout << "TokenBSL::compile() right->type() == ttMultiOp" << endl);
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
		    break;
		}
	    default:
		DBG(cout << "TokenBSL::compile() right->type() == " << (int)right->type() << endl);
		{
		    FuncCallNode *call;
		    x86::Gp &rval = right->compile(pgm, regdp);
		    if ( regdp.second && regdp.second->is_string() )
		    {
			DBG(pgm.cc.comment("TokenBSL::compile() default regdp.second->is_string() pgm.cc.call(streamout_string)"));
			call = pgm.cc.call(imm(streamout_string), FuncSignatureT<void, void *, void *>(CallConv::kIdHost));
		    }
		    else
		    {
			DBG(pgm.cc.comment("TokenBSL::compile() default pgm.cc.call(streamout_int)"));
			call = pgm.cc.call(imm(streamout_int), FuncSignatureT<void, void *, int>(CallConv::kIdHost));
		    }
		    call->setArg(0, lval);
		    call->setArg(1, rval);
		    break;
		}
	} // end switch

	DBG(cout << "TokenBSL::Compile() END" << endl);
	return lval; // return ostream
    }

    DBG(cout << "TokenBSL::compile() left->type() == " << (int)left->type()  << endl);
    DBG(cout << "TokenBSL::compile() right->type() == " << (int)right->type()  << endl);

    if ( left->type() == TokenType::ttVariable && !dynamic_cast<TokenVar *>(left)->var.type->is_numeric() )
	throw "lval is non-numeric";
    if ( right->type() == TokenType::ttVariable && !dynamic_cast<TokenVar *>(right)->var.type->is_numeric() )
	throw "rval is non-numeric";

    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(cout << "TokenBSL::compile() lval.type() " << lval.type() << " rval.type() " << rval.type() << endl);
    DBG(pgm.cc.comment("TokenBSL::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBSL::compile() pgm.cc.shl(_reg, rval.r8())"));
    pgm.cc.shl(_reg, rval.r8());

    DBG(cout << "TokenBSL::Compile() END" << endl);
    return _reg;
}

// bit shift right
x86::Gp& TokenBSR::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBSR::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBSR::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBSR::compile() pgm.cc.shr(_reg, rreg.r8())"));
    pgm.cc.shr(_reg, rval.r8());

    return _reg;
}

// bitwise or |
x86::Gp& TokenBor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBor::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBor::compile() pgm.cc.or_(_reg, rval)"));
    pgm.cc.or_(_reg, rval.r64());

    return _reg;
}

// bitwise xor ^
x86::Gp& TokenXor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenXor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenXor::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenXor::compile() pgm.cc.xor_(_reg, rval)"));
    pgm.cc.xor_(_reg, rval.r64());

    return _reg;
}

// bitwise and &
x86::Gp& TokenBand::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBand::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBand::compile() pgm.cc.and_(_reg, rval)"));
    pgm.cc.and_(_reg, rval.r64());

    return _reg;
}


// bitwise not ~
x86::Gp& TokenBnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBnot::Compile() TOP" << endl);
    if ( left )   { throw "Bitwise not has lval!"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &rreg = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBnot::compile() pgm.safemov(*ret, rreg)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, rreg);
    DBG(pgm.cc.comment("TokenBnot::compile() pgm.cc.not_(*ret)"));
    pgm.cc.not_(_reg);
    return _reg;
}

/////////////////////////////////////////////////////////////////////////////
// logic operators                                                         //
/////////////////////////////////////////////////////////////////////////////

// logical not !
x86::Gp& TokenLnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLnot::Compile() TOP" << endl);
    if ( left )   { throw "Logical not has lval!"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &rval = right->compile(pgm, regdp);
    pgm.cc.test(rval, rval); // test rval is 0
    DBG(pgm.cc.comment("TokenLnot::compile() pgm.cc.sete(_reg)"));
    pgm.cc.sete(reg.r8());   // if rval == 0, ret = 1
    return reg;	     	     // return register
}

// logical or ||
//
// Pseudocode: if (lval) return 1;  if (rval) return 1;  return 0;
//
x86::Gp& TokenLor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    Label done = pgm.cc.newLabel();	// label to skip further tests
    x86::Gp &reg  = getreg(pgm);	// get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLor::compile() pgm.cc.test(lval, lval)"));
    pgm.cc.test(lval, lval);		// test lval is 0
    DBG(pgm.cc.comment("TokenLor::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8()); 		// if lval != 0, ret = 1
    pgm.cc.jne(done);			// if lval != 0, jump to done
    pgm.cc.test(rval, rval);		// test rval is 0
    DBG(pgm.cc.comment("TokenLor::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8());		// if rval != 0, ret = 1
    pgm.cc.bind(done);			// done is here
    return reg;				// return register
}

// logical and &&
//
// Pseudocode: if (!lval) return 0;  if (!rval) return 0;  return 1;
//
x86::Gp& TokenLand::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    Label done = pgm.cc.newLabel();	// label to skip further tests
    x86::Gp &reg  = getreg(pgm);	// get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLand::compile() pgm.cc.test(lval, lval)"));
    pgm.cc.test(lval, lval);		// test lval is 0
    DBG(pgm.cc.comment("TokenLand::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8()); 		// if lval != 0, ret = 1
    pgm.cc.je(done);			// if lval == 0, jump to done
    pgm.cc.test(rval, rval);		// test rval is 0
    DBG(pgm.cc.comment("TokenLand::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8());		// if rval != 0, ret = 1
    pgm.cc.bind(done);			// done is here
    return reg;				// return register
}


/////////////////////////////////////////////////////////////////////////////
// comparison operators                                                    //
/////////////////////////////////////////////////////////////////////////////


// Equal to: ==
x86::Gp& TokenEquals::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenEquals::Compile() TOP" << endl);
    if ( !left )  { throw "= missing lval operand"; }
    if ( !right ) { throw "= missing rval operand"; } 
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(cout << "TokenEquals: lval.size() " << lval.size() << " rval.size() " << rval.size() << endl);
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.cc.sete(reg)"));
    pgm.cc.sete(reg.r8());
    return reg;
}

// Not equal to: !=
x86::Gp& TokenNotEq::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenNotEq::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.cc.setne(reg)"));
    pgm.cc.setne(reg.r8());
    return reg;
}

// Less than: <
x86::Gp& TokenLT::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLT::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    DBG(pgm.cc.comment("TokenLT::compile() reg = getreg(pgm)"));
    x86::Gp &reg  = getreg(pgm); // get clean register
    DBG(pgm.cc.comment("TokenLT::compile() lval = left->compile(pgm, regdp)"));
    x86::Gp &lval = left->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLT::compile() rval = right->compile(pgm, regdp)"));
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenLT::compile() pgm.cc.setl(reg)"));
    pgm.cc.setl(reg.r8());
    return reg;
}

// Less than or equal to: <=
x86::Gp& TokenLE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLE::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenLE::compile() pgm.cc.setle(_reg)"));
    pgm.cc.setle(reg.r8());
    return reg;
}

// Greater than: >
x86::Gp& TokenGT::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenGT::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenGT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenGT::compile() pgm.cc.setg(reg)"));
    pgm.cc.setg(reg.r8());
    return reg;
}

// Greater than or equal to: >=
x86::Gp& TokenGE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenGE::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenGE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenGE::compile() pgm.cc.setge(reg)"));
    pgm.cc.setge(reg.r8());
    return reg;
}


// Greater than gives 1, less than gives -1, equal to gives 0 (<=>)
x86::Gp& Token3Way::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "Token3Way::Compile() TOP" << endl);
    Label done = pgm.cc.newLabel();	// label to skip further tests
    Label sign = pgm.cc.newLabel();	// label to negate _reg (make negative)
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, regdp);
    x86::Gp &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("Token3Way::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);

    pgm.cc.setg(reg.r8());	// set _reg to 1 if >
    pgm.cc.jg(done);		// if >, jump to done
    pgm.cc.setl(reg.r8());	// set _reg to 1 if <
    pgm.cc.jl(sign);		// if <, jump to negate
    pgm.cc.xor_(reg, reg);	// _reg = 0
    pgm.cc.bind(sign);
    pgm.cc.neg(reg);		// _reg ? 1 : -1
    pgm.cc.bind(done); 		// done
    return reg;
}


// access structure/class member: struct.member
x86::Gp& TokenDot::compile(Program &pgm, regdefp_t &regdp)
{
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
    DBG(cout << "TokenDot::compile() accessing " << tvl->var.name << '.' << tvr->str << endl);
    // get offset
    ssize_t ofs = ((DataDefSTRUCT *)tvl->var.type)->m_offset(tvr->str);
    if ( ofs == -1 )
	throw "Unidentified member";
    // get left register
    DBG(pgm.cc.comment("TokenDot::compile() tvl->getreg(pgm)"));
    x86::Gp &lval = tvl->getreg(pgm);
    DataDef *mtype = ((DataDefSTRUCT *)tvl->var.type)->m_type(tvr->str);
    DBG(pgm.cc.comment("TokenDot::compile() _reg= mtype->newreg(tvr->str)"));
    // get new register of appropriate size
    _reg = mtype->newreg(pgm.cc, tvr->str.c_str());
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

    regdp.first  = &_reg;
    regdp.second = mtype;
    DBG(pgm.cc.comment("TokenDot::compile() mtype->name:"));
    DBG(pgm.cc.comment(mtype->name.c_str()));
    return _reg;
}




// load variable into register
x86::Gp& TokenVar::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenVar::compile() reg = getreg()"));
    x86::Gp &reg = getreg(pgm);

    if ( !regdp.second )
	regdp.second = _datatype;

    if ( regdp.first )
    {
	DBG(pgm.cc.comment("TokenVar::compile() safemov(*ret, reg)"));
	pgm.safemov(*regdp.first, reg);
	return *regdp.first;
    }

    return reg;
}

// load integer into register
x86::Gp& TokenInt::compile(Program &pgm, regdefp_t &regdp)
{
    if ( regdp.first )
    {
	DBG(pgm.cc.comment("TokenInt::compile() cc.mov(*ret, value)"));
	pgm.cc.mov(*regdp.first, _token);
	return *regdp.first;
    }
    DBG(cout << "TokenInt::compile[" << (uint64_t)this << "]() value: " << (int)_token << endl);
    return getreg(pgm);
/*
    DBG(pgm.cc.comment("TokenInt::compile() mov(_reg, value)"));
    _reg = pgm.cc.newGpq();
    pgm.cc.mov(_reg, _token);
    return _reg;
*/
/*
    if ( ret )
    {
	pgm.cc.comment("TokenInt::compile() reg = getreg()");
	x86::Gp &reg = getreg(pgm);
	if ( &reg == ret )
	    pgm.cc.comment("TokenInt::compile() reg == *ret");
	else
	{
	    pgm.cc.comment("TokenInt::compile() safemov(*ret, reg)");
	    pgm.safemov(*ret, reg);
	}
    }
*/
}

// compile a return statement
x86::Gp& TokenRETURN::compile(Program &pgm, regdefp_t &regdp)
{
    pgm.tkFunction->cleanup(pgm.cc);

    if ( returns )
    {
	x86::Gp &reg = returns->compile(pgm, regdp);
	pgm.cc.ret(reg);
	return reg;
    }
    pgm.cc.ret();

    return _reg;
}

// compile a break statement
x86::Gp& TokenBREAK::compile(Program &pgm, regdefp_t &regdp)
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
x86::Gp& TokenCONT::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !pgm.loopstack.empty() )
    {
	DBG(pgm.cc.comment("CONTINUE"));
	pgm.cc.jmp(*pgm.loopstack.top().first);
    }
    return _reg;
}

// compile an if statement
x86::Gp& TokenIF::compile(Program &pgm, regdefp_t &regdp)
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
    x86::Gp &reg = condition->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenIF::compile() pgm.cc.test(reg, reg)"));
    pgm.cc.test(reg, reg);			// compare to zero
    DBG(pgm.cc.comment("TokenIF::compile() pgm.cc.je(else/tail)"));
    pgm.cc.je(elsestmt ? elsedo : iftail);	// jump appropriately

    DBG(cout << "TokenIF::compile() calling statement->compile(pgm, regdp)" << endl);
    pgm.cc.bind(thendo);
    statement->compile(pgm, regdp); // execute if statement(s) if condition met
    if ( elsestmt )			// do we have an else?
    {
	pgm.cc.jmp(iftail);		// jump to tail after executing if statements
	pgm.cc.bind(elsedo);		// bind elsedo label
	DBG(cout << "TokenIF::compile() calling elsestmt->compile(pgm, regdp)" << endl);
	elsestmt->compile(pgm, regdp); 	// execute else condition
    }
    pgm.cc.bind(iftail);		// bind if tail

    pgm.ifstack.pop();			// pop labels from ifstack
    DBG(std::cout << "TokenIF::compile() END" << std::endl);

    return reg;
}

x86::Gp& TokenDO::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenDO::compile() TOP" << std::endl);
    Label dotop  = pgm.cc.newLabel();	// label for top of loop
    Label dodo   = pgm.cc.newLabel();	// label for loop action
    Label dotail = pgm.cc.newLabel();	// label for tail of loop

    pgm.loopstack.push(make_pair(&dotop, &dotail)); // push labels onto loopstack
    pgm.cc.bind(dotop);			// label the top of the loop
    DBG(cout << "TokenDO::compile() calling statement->compile(pgm, regdp)" << endl);
    statement->compile(pgm, regdp); 	// execute loop's statement(s)
    x86::Gp &reg = condition->compile(pgm, regdp); // get condition result
    pgm.cc.test(reg, reg);		// compare to zero
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
x86::Gp& TokenWHILE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenWHILE::compile() TOP" << std::endl);
    Label whiletop  = pgm.cc.newLabel();	// label for top of loop
    Label whiledo   = pgm.cc.newLabel();	// label for loop action
    Label whiletail = pgm.cc.newLabel();	// label for tail of loop

    pgm.loopstack.push(make_pair(&whiletop, &whiletail)); // push labels onto loopstack
    pgm.cc.bind(whiletop);			// label the top of the loop
    x86::Gp &reg = condition->compile(pgm, regdp);// get condition result
    pgm.cc.test(reg, reg);			// compare to zero
    pgm.cc.je(whiletail);			// if zero, jump to end

    DBG(cout << "TokenWHILE::compile() calling statement->compile(pgm, regdp)" << endl);
    pgm.cc.bind(whiledo);			// bind action label
    statement->compile(pgm, regdp); 		// execute loop's statement(s)
    pgm.cc.jmp(whiletop);			// jump back to top
    pgm.cc.bind(whiletail);			// bind while tail

    pgm.loopstack.pop();			// pop labels from loopstack
    DBG(std::cout << "TokenWHILE::compile() END" << std::endl);

    return reg;
}

x86::Gp& TokenFOR::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenFOR::compile() TOP" << std::endl);
    Label fortop  = pgm.cc.newLabel();		// label for top of loop
    Label forcont = pgm.cc.newLabel();		// label for continue statement
    Label fortail = pgm.cc.newLabel();		// label for tail of loop

    pgm.loopstack.push(make_pair(&forcont, &fortail)); // push labels onto loopstack
    initialize->compile(pgm, regdp); 		// execute loop's initializer statement
    pgm.cc.bind(fortop);			// label the top of the loop
    x86::Gp &reg = condition->compile(pgm, regdp); // get condition result
    pgm.cc.test(reg, reg);			// compare to zero
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
