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
#include "tokens.h"
#include "datadef.h"
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
}

void streamout_string(std::ostream &os, std::string &s)
{
    os << s;
}

void streamout_int(std::ostream &os, int i)
{
    DBG(std::cout << "streamout_int: << " << i << std::endl);
    os << i;
}

void streamout_intptr(std::ostream &os, int *i)
{
    if ( !i ) { std::cerr << "ERROR: streamout_intptr: NULL!" << std::endl; return; }
    DBG(std::cout << "streamout_intptr: << " << *i << std::endl);
    os << *i;
}

// simple for now, should have different versions for signed vs unsigned
// small to big vs big to small, etc
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

void Program::safecmp(x86::Gp &lval, x86::Gp &rval)
{
    if ( lval.size() != rval.size() )
	cc.cmp(lval.r64(), rval.r64());
    else
	cc.cmp(lval, rval);
}

void Program::_compiler_init()
{
    static FileLogger logger(stdout);

    logger.setFlags(FormatOptions::kFlagDebugRA | FormatOptions::kFlagMachineCode | FormatOptions::kFlagDebugPasses);

    code.reset();
    code.init(jit.codeInfo());
    code.setLogger(&logger);
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

x86::Gp& TokenCallFunc::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
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
    if ( !ret )
	getreg(pgm); // assign _reg if not provided

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
		    x86::Gp &p = tn->compile(pgm);
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
    if ( ret )
    {
	call->setRet(0, *ret);
	return *ret;
    }
#endif
    else
    if ( func->returns.type() != DataType::dtVOID )
    {
	call->setRet(0, _reg);
    }

    return _reg;
}

x86::Gp& TokenCpnd::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") TOP" << endl);
    x86::Gp *regp = &_reg;
    for ( vector<TokenStmt *>::iterator vti = statements.begin(); vti != statements.end(); ++vti )
    {
	regp = &(*vti)->compile(pgm, ret, l_true, l_false);
    }
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") END" << endl);

    return *regp;
}

// compile the "program" token, which contains all initilization / non-function statements
x86::Gp& TokenProgram::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
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
	(*si)->compile(pgm);
    }

    pgm.tkFunction->cleanup(pgm.cc);	// cleanup stack
    pgm.cc.ret();			// always add return in case source doesn't have one
    pgm.cc.endFunc();		// end function

    pgm.tkFunction->clear_regmap(); // clear register map

    DBG(cout << "TokenProgram::compile(" << (uint64_t)this << ") END" << endl);

    return _reg;
}

x86::Gp& TokenBase::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenStmt::compile(" << (void *)this << " type: " << (int)type() << (ret ? " ret=true" : "") << ") TOP" << endl);
    switch(type())
    {
	case TokenType::ttOperator:
	    DBG(cout << "TokenOperator::compile(" << (char)get() << ')' << endl);
	    return dynamic_cast<TokenOperator *>(this)->compile(pgm, ret, l_true, l_false);
	case TokenType::ttMultiOp:
	    DBG(cout << "TokenMultiOp::compile()" << endl);
	    return dynamic_cast<TokenMultiOp *>(this)->compile(pgm, ret, l_true, l_false);
	case TokenType::ttIdentifier:
	    DBG(cout << "TokenStmt::compile() TokenIdent(" << ((TokenIdent *)this)->str << ')' << endl);
	    break;
	case TokenType::ttKeyword:
	    return dynamic_cast<TokenKeyword *>(this)->compile(pgm);
	case TokenType::ttBaseType:
	    DBG(cout << "TokenStmt::compile() TokenBaseType(" << ((TokenBaseType *)this)->definition.name << ')' << endl);
	    break;
	case TokenType::ttInteger:
	    DBG(cout << "TokenStmt::compile() TokenInt(" << val() << ')' << endl);
	    return dynamic_cast<TokenInt *>(this)->compile(pgm, ret, l_true, l_false);
	case TokenType::ttVariable:
	    DBG(cout << "TokenStmt::compile() TokenVar(" << dynamic_cast<TokenVar *>(this)->var.name << ')' << endl);
	    return dynamic_cast<TokenVar *>(this)->compile(pgm, ret, l_true, l_false);
	case TokenType::ttCallFunc:
	    return dynamic_cast<TokenCallFunc *>(this)->compile(pgm, ret, l_true, l_false);
	case TokenType::ttDeclare:
	    return dynamic_cast<TokenDecl *>(this)->compile(pgm);
	case TokenType::ttFunction:
	    return dynamic_cast<TokenFunc *>(this)->compile(pgm);
	case TokenType::ttStatement:
	    // ttStatement should not be used anywhere
	    throw "TokenStmt::compile() tb->type() == TokenType::ttStatement";
	case TokenType::ttCompound:
	    return dynamic_cast<TokenCpnd *>(this)->compile(pgm);
	case TokenType::ttProgram:
	    return dynamic_cast<TokenProgram *>(this)->compile(pgm);
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

x86::Gp& TokenDecl::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenDecl::compile(" << var.name << ") TOP" << endl);

    if ( initialize )
	initialize->compile(pgm);

    DBG(cout << "TokenDecl::compile(" << var.name << ") END" << endl);

    return _reg;
}

x86::Gp& TokenFunc::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
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
	    x86::Gp &reg = getreg(pgm.cc, (*vvi));
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
	(*si)->compile(pgm);
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

    DBG(cout << endl << endl << "Program::compile() start" << endl << endl);
    _compiler_init();

    try
    {
	while ( !ast.empty() )
	{
	    tb = ast.front();
	    DBG(cout << "Program::compile(" << (void *)tb << ')' << endl);
	    ast.pop();
	    tb->compile(*this);
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
x86::Gp& TokenInc::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
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
x86::Gp& TokenDec::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
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
x86::Gp& TokenAssign::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenAssign::compile() TOP" << endl);
    TokenVar *tvl, *tvr;
    TokenCallFunc *tcr;

    if ( !left )
	throw "Assignment with no lval";
    if ( !right )
	throw "Assignment with no rval";
    if ( left->type() != TokenType::ttVariable )
	throw "Assignment on a non-variable lval";

    tvl = dynamic_cast<TokenVar *>(left);
    DBG(cout << "TokenAssign::compile() assignment to " << tvl->var.name << endl);

    // get left register
    x86::Gp &lreg = tvl->getreg(pgm);

    switch(right->type())
    {
	case TokenType::ttVariable:
	    tvr = dynamic_cast<TokenVar *>(right);
	    DBG(cout << "TokenAssign::compile() assignment from " << tvr->var.name << endl);
	    if ( &tvl->var == &tvr->var )
	    {
		DBG(cout << "TokenAssign::compile() assigning variable to itself? nothing to do!" << endl);
		break;
	    }
	    if ( tvl->var.type->is_numeric() && tvr->var.type->is_numeric() )
	    {
		DBG(cout << "TokenAssign::compile() numeric to numeric" << endl);
		x86::Gp &rreg = tvr->getreg(pgm); //pgm.tkFunction->getreg(pgm.cc, &tvr->var);
		pgm.safemov(lreg, rreg);
		tvl->var.modified();
		tvl->putreg(pgm); //pgm.tkFunction->putreg(pgm.cc, &tvl->var);
		break;
	    }
	    if ( tvl->var.type->is_string() && tvr->var.type->is_string() )
	    {
		DBG(cout << "TokenAssign::compile() string to string" << endl);
		DBG(cout << "TokenAssign::compile() will call " << tvl->var.name << '('
		    << (tvl->var.data ? ((string *)(tvl->var.data))->c_str() : "") << ").assign[" << (uint64_t)string_assign << "](" << tvr->var.name
		    << '(' << (tvr->var.data ? ((string *)(tvr->var.data))->c_str() : "") << ')' << endl);

		x86::Gp &rreg = tvr->getreg(pgm); //pgm.tkFunction->getreg(pgm.cc, &tvr->var);
		DBG(pgm.cc.comment("string_assign"));
		FuncCallNode* call = pgm.cc.call(imm(string_assign), FuncSignatureT<void, const char*, const char *>(CallConv::kIdHost));
		call->setArg(0, lreg);
		call->setArg(1, rreg);
		tvl->var.modified();
		tvl->putreg(pgm); // pgm.tkFunction->putreg(pgm.cc, &tvl->var);
		break;
	    }
	    if ( tvl->var.type->type() != tvr->var.type->type() )
		throw "Variable types do not match";
	    else
		throw "Unimplemented assignment";
	case TokenType::ttInteger:
	    if ( tvl->var.type->is_numeric() )
	    {
		DBG(cout << "TokenAssign::compile() pgm.cc.mov(" << tvl->var.name << ".reg, " << right->val() << ')' << endl);
		DBG(pgm.cc.comment("TokenAssign::compile() pgm.cc.mov(lreg, right->val)"));
		pgm.cc.mov(lreg, right->val());
		tvl->var.modified();
		tvl->putreg(pgm); // pgm.tkFunction->putreg(pgm.cc, &tvl->var);
		break;
	    }
	    throw "Variable non-numeric";
	case TokenType::ttCallFunc:
	    tcr = dynamic_cast<TokenCallFunc *>(right);
	    if ( (tvl->var.type->is_numeric() && tcr->returns()->is_numeric())
	    ||   (tvl->var.type == tcr->returns()) )
	    {
		tcr->compile(pgm, &lreg);
		tvl->var.modified();
		tvl->putreg(pgm); // pgm.tkFunction->putreg(pgm.cc, &tvl->var);
		break;
	    }
	    throw "Return type not compatible";
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	    {
		// temp for right side
//		x86::Gp rreg = pgm.cc.newGpq();
//		right->compile(pgm, &rreg, l_true, l_false);
//		pgm.safemov(lreg, rreg);
		x86::Gp &rreg = right->compile(pgm, NULL, l_true, l_false);
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
    return pgm.tkFunction->getreg(pgm.cc, &var);
}

// variable also needs to be able to write the register back to variable
void TokenVar::putreg(Program &pgm)
{
    pgm.tkFunction->putreg(pgm.cc, &var);
}

// function needs similar handling to variable
x86::Gp &TokenCallFunc::getreg(Program &pgm)
{
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
    return _reg;
}

// Manage registers for use on local as well as global variables
x86::Gp &TokenCpnd::getreg(x86::Compiler &cc, Variable *var)
{
    std::map<Variable *, asmjit::x86::Gp>::iterator rmi;

    if ( (rmi=register_map.find(var)) != register_map.end() )
    {
	DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::getreg(" << var->name << ") found" << std::endl);
	// copy global variable to register -- needs to happen every time we need to access a global
	if ( var->is_global() && var->data )
	{
	    DBG(cc.comment("TokenCpnd::getreg(global) cc.mov(reg, var->data)"));
	    switch(var->type->type())
	    {
		case DataType::dtCHAR:    cc.mov(rmi->second, asmjit::x86::byte_ptr((uintptr_t)var->data));  break;
		case DataType::dtBOOL:    cc.mov(rmi->second, asmjit::x86::byte_ptr((uintptr_t)var->data));  break;
		case DataType::dtINT64:   cc.mov(rmi->second, asmjit::x86::qword_ptr((uintptr_t)var->data)); break;
		case DataType::dtINT16:   cc.mov(rmi->second, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
		case DataType::dtINT24:   cc.mov(rmi->second, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
		case DataType::dtINT32:   cc.mov(rmi->second, asmjit::x86::dword_ptr((uintptr_t)var->data)); break;
		case DataType::dtUINT8:   cc.mov(rmi->second, asmjit::x86::byte_ptr((uintptr_t)var->data));  break;
		case DataType::dtUINT16:  cc.mov(rmi->second, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
		case DataType::dtUINT24:  cc.mov(rmi->second, asmjit::x86::word_ptr((uintptr_t)var->data));  break;
		case DataType::dtUINT32:  cc.mov(rmi->second, asmjit::x86::dword_ptr((uintptr_t)var->data)); break;
		case DataType::dtUINT64:  cc.mov(rmi->second, asmjit::x86::qword_ptr((uintptr_t)var->data)); break;
		default:		  cc.mov(rmi->second, asmjit::imm(var->data));			     break;
	    } // switch
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
		std::cerr << "unsupported type: " << (int)var->type->type() << std::endl;
		std::cerr << "reftype: " << (int)var->type->reftype() << std::endl;
		throw "TokenCpnd()::getreg() unsupported type on stack";
		
	} // switch
    }
    else
    {
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
    }
    var->flags |= vfREGSET;

    return getreg(cc, var);
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
    cc.comment("TokenCpnd::putreg() calling cc.mov(var->data, reg)");
    switch(var->type->type())
    {
	case DataType::dtCHAR:    cc.mov(asmjit::x86::byte_ptr((uintptr_t)var->data),  rmi->second); break;
	case DataType::dtBOOL:    cc.mov(asmjit::x86::byte_ptr((uintptr_t)var->data),  rmi->second); break;
	case DataType::dtINT64:   cc.mov(asmjit::x86::qword_ptr((uintptr_t)var->data), rmi->second); break;
	case DataType::dtINT16:   cc.mov(asmjit::x86::word_ptr((uintptr_t)var->data),  rmi->second); break;
	case DataType::dtINT24:   cc.mov(asmjit::x86::word_ptr((uintptr_t)var->data),  rmi->second); break;
	case DataType::dtINT32:   cc.mov(asmjit::x86::dword_ptr((uintptr_t)var->data), rmi->second); break;
	case DataType::dtUINT8:   cc.mov(asmjit::x86::byte_ptr((uintptr_t)var->data),  rmi->second); break;
	case DataType::dtUINT16:  cc.mov(asmjit::x86::word_ptr((uintptr_t)var->data),  rmi->second); break;
	case DataType::dtUINT24:  cc.mov(asmjit::x86::word_ptr((uintptr_t)var->data),  rmi->second); break;
	case DataType::dtUINT32:  cc.mov(asmjit::x86::dword_ptr((uintptr_t)var->data), rmi->second); break;
	case DataType::dtUINT64:  cc.mov(asmjit::x86::qword_ptr((uintptr_t)var->data), rmi->second); break;
	default: DBG(std::cerr << "TokenCpnd::putreg() unsupported numeric type " << (int)var->type->type() << std::endl); break;
    }
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
x86::Gp& TokenAdd::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenAdd::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    _reg = pgm.cc.newGpq();
    DBG(pgm.cc.comment("TokenAdd::compile() pgm.safemov(_reg, lval)"));
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenAdd::compile() pgm.cc.add(_reg, rval)"));
    pgm.cc.add(_reg, rval.r64());

    return _reg;
}

// subtract two integers
x86::Gp& TokenSub::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenSub::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( ret )    { throw "TokenSub::compile() ret is set"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenSub::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenSub::compile() pgm.cc.sub(_reg, rval)"));
    pgm.cc.sub(_reg, rval.r64());

    return _reg;
}

// multiply two integers
x86::Gp& TokenMul::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenMul::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenMul::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenMul::compile() pgm.cc.imud(_reg, rval)"));
    pgm.cc.imul(_reg, rval.r64());

    return _reg;
}

// divide two integers
x86::Gp& TokenDiv::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenDiv::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; } 
    if ( !right ) { throw "!= missing rval operand"; }

    x86::Gp remainder = pgm.cc.newInt64("TokenDiv::remainder");
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    pgm.cc.xor_(remainder, remainder);
    _reg = pgm.cc.newGpq();
    DBG(pgm.cc.comment("TokenDiv::compile() pgm.safemov(_reg, lval)"));
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenDiv::compile() pgm.cc.div(remainder, _reg, rval)"));
    pgm.cc.idiv(remainder, _reg, rval.r64());
    return _reg;
}

// modulus
x86::Gp& TokenMod::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenMod::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &remainder = (_reg = pgm.cc.newInt64("remainder"));
    x86::Gp quotent = pgm.cc.newInt64("quotent");
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
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
x86::Gp& TokenBSL::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenBSL::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenBSL::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBSL::compile() pgm.cc.shl(_reg, rval.r8())"));
    pgm.cc.shl(_reg, rval.r8());

    return _reg;
}

// bit shift right
x86::Gp& TokenBSR::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenBSR::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenBSR::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBSR::compile() pgm.cc.shr(_reg, rreg.r8())"));
    pgm.cc.shr(_reg, rval.r8());

    return _reg;
}

// bitwise or |
x86::Gp& TokenBor::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenBor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenBor::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBor::compile() pgm.cc.or_(_reg, rval)"));
    pgm.cc.or_(_reg, rval.r64());

    return _reg;
}

// bitwise xor ^
x86::Gp& TokenXor::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenXor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenXor::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenXor::compile() pgm.cc.xor_(_reg, rval)"));
    pgm.cc.xor_(_reg, rval.r64());

    return _reg;
}

// bitwise and &
x86::Gp& TokenBand::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenBand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenBand::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBand::compile() pgm.cc.and_(_reg, rval)"));
    pgm.cc.and_(_reg, rval.r64());

    return _reg;
}


// bitwise not ~
x86::Gp& TokenBnot::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenBnot::Compile() TOP" << endl);
    if ( left )   { throw "Bitwise not has lval!"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &rreg = right->compile(pgm, NULL, l_true, l_false);
    if ( !ret )   { _reg = pgm.cc.newGpq(); ret = &_reg; }
    DBG(pgm.cc.comment("TokenBnot::compile() pgm.safemov(*ret, rreg)"));
    pgm.safemov(*ret, rreg);
    DBG(pgm.cc.comment("TokenBnot::compile() pgm.cc.not_(*ret)"));
    pgm.cc.not_(*ret);
    return *ret;
}

/////////////////////////////////////////////////////////////////////////////
// logic operators                                                         //
/////////////////////////////////////////////////////////////////////////////

// logical not !
x86::Gp& TokenLnot::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenLnot::Compile() TOP" << endl);
    if ( left )   { throw "Logical not has lval!"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( ret )    { throw "TokenLnot::compile() ret is set"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    pgm.cc.test(rval, rval); // test rval is 0
    DBG(pgm.cc.comment("TokenLnot::compile() pgm.cc.sete(_reg)"));
    pgm.cc.sete(reg.r8());   // if rval == 0, ret = 1
    return reg;	     	     // return register
}

// logical or ||
//
// Pseudocode: if (lval) return 1;  if (rval) return 1;  return 0;
//
x86::Gp& TokenLor::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenLor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( ret )    { throw "TokenLor::compile() ret is set"; }
    Label done = pgm.cc.newLabel();	// label to skip further tests
    x86::Gp &reg  = getreg(pgm);	// get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
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
x86::Gp& TokenLand::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenLand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( ret )    { throw "TokenLand::compile() ret is set"; }
    Label done = pgm.cc.newLabel();	// label to skip further tests
    x86::Gp &reg  = getreg(pgm);	// get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
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
x86::Gp& TokenEquals::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenEquals::Compile() TOP" << endl);
    if ( !left )  { throw "= missing lval operand"; }
    if ( !right ) { throw "= missing rval operand"; } 
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(cout << "TokenEquals: lval.size() " << lval.size() << " rval.size() " << rval.size() << endl);
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.cc.sete(reg)"));
    pgm.cc.sete(reg.r8());
    return reg;
}

// Not equal to: !=
x86::Gp& TokenNotEq::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenNotEq::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.cc.setne(reg)"));
    pgm.cc.setne(reg.r8());
    return reg;
}

// Less than: <
x86::Gp& TokenLT::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenLT::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    DBG(pgm.cc.comment("TokenLT::compile() reg = getreg(pgm)"));
    x86::Gp &reg  = getreg(pgm); // get clean register
    DBG(pgm.cc.comment("TokenLT::compile() lval = left->compile(pgm)"));
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenLT::compile() rval = right->compile(pgm)"));
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenLT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenLT::compile() pgm.cc.setl(reg)"));
    pgm.cc.setl(reg.r8());
    return reg;
}

// Less than or equal to: <=
x86::Gp& TokenLE::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenLE::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenLE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenLE::compile() pgm.cc.setle(_reg)"));
    pgm.cc.setle(reg.r8());
    return reg;
}

// Greater than: >
x86::Gp& TokenGT::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenGT::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenGT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenGT::compile() pgm.cc.setg(reg)"));
    pgm.cc.setg(reg.r8());
    return reg;
}

// Greater than or equal to: >=
x86::Gp& TokenGE::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "TokenGE::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
    DBG(pgm.cc.comment("TokenGE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenGE::compile() pgm.cc.setge(reg)"));
    pgm.cc.setge(reg.r8());
    return reg;
}


// Greater than gives 1, less than gives -1, equal to gives 0 (<=>)
x86::Gp& Token3Way::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(cout << "Token3Way::Compile() TOP" << endl);
    Label done = pgm.cc.newLabel();	// label to skip further tests
    Label sign = pgm.cc.newLabel();	// label to negate _reg (make negative)
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    x86::Gp &lval = left->compile(pgm, NULL, l_true, l_false);
    x86::Gp &rval = right->compile(pgm, NULL, l_true, l_false);
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



// load variable into register
x86::Gp& TokenVar::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(pgm.cc.comment("TokenVar::compile() reg = getreg()"));
    x86::Gp &reg = getreg(pgm);

    if ( ret )
    {
	DBG(pgm.cc.comment("TokenVar::compile() safemov(*ret, reg)"));
	pgm.safemov(*ret, reg);
	return *ret;
    }

    return reg;
}

x86::Gp& TokenInt::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    if ( ret )
    {
	DBG(pgm.cc.comment("TokenInt::compile() cc.mov(*ret, value)"));
	pgm.cc.mov(*ret, _token);
	return *ret;
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
x86::Gp& TokenRETURN::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    pgm.tkFunction->cleanup(pgm.cc);

    if ( returns )
    {
	x86::Gp &reg = returns->compile(pgm, NULL, l_true, l_false);
	pgm.cc.ret(reg);
	return reg;
    }
    pgm.cc.ret();

    return _reg;
}

// compile an if statement
x86::Gp& TokenIF::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(std::cout << "TokenIF::compile() TOP" << std::endl);
    Label iftail = pgm.cc.newLabel();	// label for tail of if
    Label thendo = pgm.cc.newLabel();	// label for then condition
    Label elsedo = pgm.cc.newLabel();	// label for else condition

    if ( !statement ) { throw "if missing statement"; }
    // perform condition check, false goes either to elsedo or iftail
    DBG(pgm.cc.comment("TokenIF::compile() reg = condition->compile()"));
    x86::Gp &reg = condition->compile(pgm, NULL, &thendo, elsestmt ? &elsedo : &iftail);
    DBG(pgm.cc.comment("TokenIF::compile() pgm.cc.test(reg, reg)"));
    pgm.cc.test(reg, reg);			// compare to zero
    DBG(pgm.cc.comment("TokenIF::compile() pgm.cc.je(else/tail)"));
    pgm.cc.je(elsestmt ? elsedo : iftail);	// jump appropriately

    DBG(cout << "TokenIF::compile() calling statement->compile(pgm)" << endl);
    pgm.cc.bind(thendo);
    statement->compile(pgm); // execute if statement(s) if condition met
    if ( elsestmt )			// do we have an else?
    {
	pgm.cc.jmp(iftail);		// jump to tail after executing if statements
	pgm.cc.bind(elsedo);		// bind elsedo label
	DBG(cout << "TokenIF::compile() calling elsestmt->compile(pgm)" << endl);
	elsestmt->compile(pgm); 	// execute else condition
    }
    pgm.cc.bind(iftail);		// bind if tail

    DBG(std::cout << "TokenIF::compile() END" << std::endl);

    return reg;
}

x86::Gp& TokenDO::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(std::cout << "TokenDO::compile() TOP" << std::endl);
    Label dotop  = pgm.cc.newLabel();	// label for top of loop
    Label dodo   = pgm.cc.newLabel();	// label for loop action
    Label dotail = pgm.cc.newLabel();	// label for tail of loop

    pgm.cc.bind(dotop);			// label the top of the loop
    DBG(cout << "TokenDO::compile() calling statement->compile(pgm)" << endl);
    statement->compile(pgm);		// execute loop's statement(s)
    x86::Gp &reg = condition->compile(pgm, NULL, &dodo, &dotail);
    pgm.cc.test(reg, reg);		// compare to zero
    pgm.cc.je(dotail);			// jump to end

    pgm.cc.bind(dodo);			// bind action label
    pgm.cc.jmp(dotop);			// jump back to top
    pgm.cc.bind(dotail);		// bind do tail

    DBG(std::cout << "TokenDO::compile() END" << std::endl);

    return reg;
}

// while ( condition ) statement;
// TODO: need way to support break and continue
x86::Gp& TokenWHILE::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(std::cout << "TokenWHILE::compile() TOP" << std::endl);
    Label whiletop  = pgm.cc.newLabel();	// label for top of loop
    Label whiledo   = pgm.cc.newLabel();	// label for loop action
    Label whiletail = pgm.cc.newLabel();	// label for tail of loop

    pgm.cc.bind(whiletop);			// label the top of the loop
    x86::Gp &reg = condition->compile(pgm, NULL, &whiledo, &whiletail);
    pgm.cc.test(reg, reg);			// compare to zero
    pgm.cc.je(whiletail);			// if zero, jump to end

    DBG(cout << "TokenWHILE::compile() calling statement->compile(pgm)" << endl);
    pgm.cc.bind(whiledo);			// bind action label
    statement->compile(pgm);			// execute loop's statement(s)
    pgm.cc.jmp(whiletop);			// jump back to top
    pgm.cc.bind(whiletail);			// bind while tail

    DBG(std::cout << "TokenWHILE::compile() END" << std::endl);

    return reg;
}

x86::Gp& TokenFOR::compile(Program &pgm, x86::Gp *ret, asmjit::Label *l_true, asmjit::Label *l_false)
{
    DBG(std::cout << "TokenFOR::compile() TOP" << std::endl);
    Label fortop  = pgm.cc.newLabel();		// label for top of loop
    Label fordo   = pgm.cc.newLabel();		// label for loop action
    Label fortail = pgm.cc.newLabel();		// label for tail of loop

    initialize->compile(pgm); 			// execute loop's initializer statement
    pgm.cc.bind(fortop);			// label the top of the loop
    x86::Gp &reg = condition->compile(pgm, NULL, &fordo, &fortail);
    pgm.cc.test(reg, reg);			// compare to zero
    pgm.cc.je(fortail);				// jump to end

    pgm.cc.bind(fordo);				// bind action label
    DBG(cout << "TokenFOR::compile() calling statement->compile(pgm)" << endl);
    statement->compile(pgm);			// execute loop's statement(s)
    increment->compile(pgm); 			// execute loop's increment statement
    pgm.cc.jmp(fortop);				// jump back to top
    pgm.cc.bind(fortail);			// bind for tail

    DBG(std::cout << "TokenFOR::compile() END" << std::endl);

    return reg;
}
