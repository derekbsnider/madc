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

void Program::safemov(x86::Gp &r1, x86::Xmm &r2)
{
   throw "safemov() unable to move xmm to gp";
}
void Program::safemov(x86::Xmm &r1, x86::Gp &r2)
{
   throw "safemov() unable to move gp to xmm";
}
void Program::safemov(x86::Xmm &r1, x86::Xmm &r2)
{
   cc.movsd(r1, r2);
}
void Program::safemov(x86::Xmm &r1, Imm &r2)
{
   throw "safemov() unable to move imm to xmm";
}

void Program::safemov(Operand &op1, int i)
{
    Operand op2 = imm(i);
    safemov(op1, op2);
}

// should handle all necessary conversions...
void Program::safemov(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() ) { throw "safemov() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() ) { throw "safemov() rval is not register or immediate"; }
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    safemov(op1.as<x86::Xmm>(), op2.as<Imm>());
	else
	    throw "safemov() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safemov(op1.as<x86::Gp>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safemov(op1.as<x86::Gp>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    cc.mov(op1.as<x86::Gp>(), op2.as<Imm>());
	else
	    throw "safemov() rval is unsupported";
    }
    else
    {
	throw "safemov() lval unsupported type";
    }
}

// simple for now, should have different versions for signed vs unsigned
// small to big vs big to small, etc, as we need to ensure that adding
// small to big doesn't leave unwanted data in the other part of the register
void Program::safeadd(x86::Gp &r1, x86::Gp &r2)
{
    switch(r1.type())
    {
	case BaseReg::kTypeGp8Lo: cc.add(r1, r2.r8Lo());  break;
	case BaseReg::kTypeGp8Hi: cc.add(r1, r2.r8Hi());  break;
	case BaseReg::kTypeGp16:  cc.add(r1, r2.r16());   break;
	case BaseReg::kTypeGp32:  cc.add(r1, r2.r32());   break;
	case BaseReg::kTypeGp64:  cc.add(r1, r2.r64());   break;
	default: throw "Program::safeadd() cannot match register types";
    }
}

void Program::safeadd(x86::Gp &r1, x86::Xmm &r2)
{
   throw "safeadd() unable to add xmm to gp";
}
void Program::safeadd(x86::Xmm &r1, x86::Gp &r2)
{
   throw "safeadd() unable to add gp to xmm";
}
void Program::safeadd(x86::Xmm &r1, x86::Xmm &r2)
{
   cc.addsd(r1, r2);
}
void Program::safeadd(x86::Xmm &r1, Imm &r2)
{
   throw "safeadd() unable to add imm to xmm";
}

void Program::safeadd(Operand &op1, int i)
{
    Operand op2 = imm(i);
    safeadd(op1, op2);
}

// should handle all necessary conversions...
void Program::safeadd(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() ) { throw "safeadd() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() ) { throw "safeadd() rval is not register or immediate"; }
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safeadd(op1.as<x86::Xmm>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safeadd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    safeadd(op1.as<x86::Xmm>(), op2.as<Imm>());
	else
	    throw "safeadd() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safeadd(op1.as<x86::Gp>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safeadd(op1.as<x86::Gp>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    cc.add(op1.as<x86::Gp>(), op2.as<Imm>());
	else
	    throw "safeadd() rval is unsupported";
    }
    else
    {
	throw "safeadd() lval unsupported type";
    }
}

// simple for now, should have different versions for signed vs unsigned
// small to big vs big to small, etc, as we need to ensure that subing
// small to big doesn't leave unwanted data in the other part of the register
void Program::safesub(x86::Gp &r1, x86::Gp &r2)
{
    switch(r1.type())
    {
	case BaseReg::kTypeGp8Lo: cc.sub(r1, r2.r8Lo());  break;
	case BaseReg::kTypeGp8Hi: cc.sub(r1, r2.r8Hi());  break;
	case BaseReg::kTypeGp16:  cc.sub(r1, r2.r16());   break;
	case BaseReg::kTypeGp32:  cc.sub(r1, r2.r32());   break;
	case BaseReg::kTypeGp64:  cc.sub(r1, r2.r64());   break;
	default: throw "Program::safesub() cannot match register types";
    }
}

void Program::safesub(x86::Gp &r1, x86::Xmm &r2)
{
   throw "safesub() unable to sub xmm to gp";
}
void Program::safesub(x86::Xmm &r1, x86::Gp &r2)
{
   throw "safesub() unable to sub gp to xmm";
}
void Program::safesub(x86::Xmm &r1, x86::Xmm &r2)
{
   cc.subsd(r1, r2);
}
void Program::safesub(x86::Xmm &r1, Imm &r2)
{
   throw "safesub() unable to sub imm to xmm";
}

void Program::safesub(Operand &op1, int i)
{
    Operand op2 = imm(i);
    safesub(op1, op2);
}

// should handle all necessary conversions...
void Program::safesub(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() ) { throw "safesub() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() ) { throw "safesub() rval is not register or immediate"; }
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safesub(op1.as<x86::Xmm>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safesub(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    safesub(op1.as<x86::Xmm>(), op2.as<Imm>());
	else
	    throw "safesub() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safesub(op1.as<x86::Gp>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safesub(op1.as<x86::Gp>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    cc.sub(op1.as<x86::Gp>(), op2.as<Imm>());
	else
	    throw "safesub() rval is unsupported";
    }
    else
    {
	throw "safesub() lval unsupported type";
    }
}

void Program::safeneg(Operand &op)
{
    if ( !op.isReg() ) { throw "safeneg() lval is not a register"; }
    if ( op.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	x86::Xmm tmp = cc.newXmm();
	throw "safeneg doesn't support xmm";
    }
    else
    if ( op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	cc.neg(op.as<x86::Gp>());
    else
	throw "safeneg() unsupported register type";
}

// perform cc.mul with size casting
void Program::safemul(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safemul() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp)) )
	throw "safemul() right operand is not a Gp register or immediate value";
    if ( op2.isImm() )
	cc.imul(op1.as<x86::Gp>(), op2.as<Imm>());
    else
	cc.imul(op1.as<x86::Gp>(), op2.as<x86::Gp>());
}

// perform cc.div with size casting
void Program::safediv(Operand &op1, Operand &op2, Operand &op3)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safediv() left operand is not a Gp register";
    if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safediv() middle operand is not a Gp register";
    if ( !op3.isReg() || !op3.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safediv() right operand is not a Gp register";
    DBG(cc.comment("safediv() cc.idiv(op1, op2, op3)"));
    cc.idiv(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64(), op3.as<x86::Gp>().r64());
}

// perform cc.shl with size casting
void Program::safeshl(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safeshl() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp)) )
	throw "safeshl() right operand is not a Gp register or immediate value";
    if ( op2.isImm() )
	cc.shl(op1.as<x86::Gp>(), op2.as<Imm>());
    else
	cc.shl(op1.as<x86::Gp>(), op2.as<x86::Gp>().r8());
}

// perform cc.shr with size casting
void Program::safeshr(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safeshr() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp)) )
	throw "safeshr() right operand is not a Gp register or immediate value";
    if ( op2.isImm() )
	cc.shr(op1.as<x86::Gp>(), op2.as<Imm>());
    else
	cc.shr(op1.as<x86::Gp>(), op2.as<x86::Gp>().r8());
}

// perform cc.or_ with size casting
void Program::safeor(Operand &op1, Operand &op2)
{
}

// perform cc.and_ with size casting
void Program::safeand(Operand &op1, Operand &op2)
{
}

// perform cc.xor_ with size casting
void Program::safexor(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safexor() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp)) )
	throw "safexor() right operand is not a Gp register or immediate value";
    if ( op2.isImm() )
	cc.xor_(op1.as<x86::Gp>(), op2.as<Imm>());
    else
	cc.xor_(op1.as<x86::Gp>(), op2.as<x86::Gp>().r8());
}

// perform cc.not_ with size casting
void Program::safenot(Operand &op)
{
}

void Program::saferet(Operand &op)
{
    if ( !op.isReg() && !op.isImm() ) { throw "saferet() operand is not register or immediate"; }
    if ( op.isReg() )
    {
	if ( op.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    cc.ret(op.as<x86::Xmm>());
	else
	if ( op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    cc.ret(op.as<x86::Gp>());
	else
	    throw "saferet() operand is not a supported register type";
    }
    else
	throw "saferet() unsupported operand";
}



// perform a test on two operands
void Program::safetest(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safetest() left operand is not a Gp register";
    if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safetest() right operand is not a Gp register";
    cc.test(op1.as<x86::Gp>(), op2.as<x86::Gp>());
}

void Program::safesete(Operand &op)
{
    if ( !op.isReg() || !op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safesete() operand is not a Gp register";
    cc.sete(op.as<x86::Gp>().r8());
}

void Program::safesetg(Operand &op)
{
    if ( !op.isReg() || !op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safesetg() operand is not a Gp register";
    cc.setg(op.as<x86::Gp>().r8());
}

void Program::safesetge(Operand &op)
{
    if ( !op.isReg() || !op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safesetge() operand is not a Gp register";
    cc.setge(op.as<x86::Gp>().r8());
}

void Program::safesetl(Operand &op)
{
    if ( !op.isReg() || !op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safesetl() operand is not a Gp register";
    cc.setl(op.as<x86::Gp>().r8());
}

void Program::safesetle(Operand &op)
{
    if ( !op.isReg() || !op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safesetle() operand is not a Gp register";
    cc.setle(op.as<x86::Gp>().r8());
}

void Program::safesetne(Operand &op)
{
    if ( !op.isReg() || !op.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safesetne() operand is not a Gp register";
    cc.setne(op.as<x86::Gp>().r8());
}



// compare two registers even if they are different sizes
void Program::safecmp(x86::Gp &lval, x86::Gp &rval)
{
    if ( lval.size() != rval.size() )
	cc.cmp(lval.r64(), rval.r64());
    else
	cc.cmp(lval, rval);
}

void Program::safecmp(x86::Gp &r1, x86::Xmm &r2)
{
   throw "safecmp() unable to cmp xmm to gp";
}
void Program::safecmp(x86::Xmm &r1, x86::Gp &r2)
{
   throw "safecmp() unable to cmp gp to xmm";
}
void Program::safecmp(x86::Xmm &r1, x86::Xmm &r2)
{
   cc.cmpsd(r1, r2, 0);
}

void Program::safecmp(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() ) { throw "safecmp() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() ) { throw "safecmp() rval is not register or immediate"; }
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safecmp(op1.as<x86::Xmm>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safecmp(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    throw "safecmp() unable to cmp imm to xmm";
	else
	    throw "safecmp() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safecmp(op1.as<x86::Gp>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safecmp(op1.as<x86::Gp>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    cc.cmp(op1.as<x86::Gp>(), op2.as<Imm>());
	else
	    throw "safecmp() rval is unsupported";
    }
    else
    {
	throw "safecmp() lval unsupported type";
    }
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

Operand &TokenCallFunc::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCallFunc::compile(" << var.name << ") TOP" << endl);
    DBG(pgm.cc.comment("TokenCallFunc start"));

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

    // build arguments
    FuncDef *func = (FuncDef *)method->returns.type;
    FuncSignatureBuilder funcsig(CallConv::kIdHost);
    std::vector<Operand> params;
    DataDef *ptype;
    uint32_t _argc;
    TokenBase *tn;

    if ( !regdp.second )
	regdp.second = &func->returns;

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

//#if OBJECT_SUPPORT
    // pass along object ("this") as first argument if appropriate
    if ( regdp.objreg && func->parameters.size() )
    {
	ptype = func->parameters[0];
	// we need to add support for reference types
	if ( 1 /*ptype->is_object()*/ )
	{
	    funcsig.addArgT<void *>();
	    params.push_back(*regdp.objreg);
	    DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(*regdp.objreg"));
	}
	else
	{
	    DBG(std::cout << "TokenCallFunc::compile() got obj param, but param[0] is not an object: " << (int)ptype->type() << std::endl);
	    DBG(pgm.cc.comment("TokenCallFunc::compile() got obj param, but param[0] is not an object"));
	}
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
	throw "TokenCallFunc::compile() called with too many parameters";
    }

    for ( size_t i = 0; i < argc(); ++i )
    {
	regdefp_t funcrdp;
	ptype = func->parameters[i];
	tn = parameters[i];

	funcrdp.objreg = NULL; // should this be regdp.objreg?
	funcrdp.second = ptype;
	_reg = ptype->newreg(pgm.cc);
	funcrdp.first = &_reg;	
	Operand &tnreg = tn->compile(pgm, funcrdp);
	if ( !funcrdp.second )
	    throw "Failed to detemine type of rval";
	if ( ptype->is_numeric() && !funcrdp.second->is_numeric() )
	{
	    DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)funcrdp.second->type() << endl);
	    throw "Expecting numeric argument";
	}
	if ( ptype->is_string() && !funcrdp.second->is_string() )
	    throw "Expecting string argument";
	if ( ptype->is_object() )
	{
	    if ( !funcrdp.second->is_object() )
		throw "Expecting object argument";
	    // check for has_ostream / has_istream
	    if ( ptype->rawtype() != funcrdp.second->rawtype() )
		throw "Object type mismatch";
	}
	DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(tv->getreg(pgm))"));
	params.push_back(tnreg); // params.push_back(pgm.tkFunction->getreg(pgm.cc, &tv->var));
	// could probably use a tv->var.addArgT(funcsig) method
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

    for ( gvi = params.begin(); gvi != params.end(); ++gvi )
    {
	DBG(std::cout << "TokenCallFunc::compile(call->setArg(" << _argc << ", reg)" << endl);
	
	if ( gvi->isReg() )
	{
	    if ( gvi->as<BaseReg>().isGroup(BaseReg::kGroupVec) )
		call->setArg(_argc++, gvi->as<x86::Xmm>());
	    else
	    if ( gvi->as<BaseReg>().isGroup(BaseReg::kGroupGp) )
		call->setArg(_argc++, gvi->as<x86::Gp>());
	    else
		throw "TokenCallFunc::compile() unexpected parameter Operand";
	}
	else
	if ( gvi->isImm() )
	    call->setArg(_argc++, gvi->as<Imm>());
    }

    DBG(std::cout << "TokenCallFunc::compile() END" << std::endl);
#if 1
    // handle return value
    if ( regdp.first )
    {
	if ( !regdp.first->isReg() )
	    throw "TokenCallFunc::compile() regdp.first->isReg() is FALSE";
	if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    call->setRet(0, regdp.first->as<x86::Xmm>());
	else
	if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    call->setRet(0, regdp.first->as<x86::Gp>());
	return *regdp.first;
    }
    else
#endif
    if ( func->returns.type() != DataType::dtVOID )
    {
	call->setRet(0, _reg);
	regdp.first = &_reg;
    }

    return _reg;
}

Operand &TokenCpnd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") TOP" << endl);
    Operand *regp = &_reg;
    for ( vector<TokenStmt *>::iterator vti = statements.begin(); vti != statements.end(); ++vti )
    {
	regp = &(*vti)->compile(pgm, regdp);
    }
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") END" << endl);

    return *regp;
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
	    DBG(cout << "TokenStmt::compile() TokenInt(" << val() << ')' << endl);
	    return dynamic_cast<TokenInt *>(this)->compile(pgm, regdp);
	case TokenType::ttReal:
	    DBG(cout << "TokenStmt::compile() TokenReal(" << ((TokenReal *)this)->dval() << ')' << endl);
	    return dynamic_cast<TokenReal *>(this)->compile(pgm, regdp);
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

Operand &TokenDecl::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDecl::compile(" << var.name << ") TOP" << endl);

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
Operand &TokenDec::compile(Program &pgm, regdefp_t &regdp)
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
Operand &TokenAssign::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAssign::compile() TOP" << endl);
    TokenVar *tvl; //, *tvr;
//  TokenCallFunc *tcr;
    TokenDot *tdot = NULL;
    x86::Gp *regp;
    DataDef *ltype;

    if ( !left )
	throw "Assignment with no lval";
    if ( !right )
	throw "Assignment with no rval";

    DBG(pgm.cc.comment("TokenAssign start"));

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
#if 1
    else
    if ( left->type() == TokenType::ttMember )
    {
	TokenMember *tvm = dynamic_cast<TokenMember *>(left);
	ltype = tvm->var.type;
	DBG(cout << "TokenAssign::compile() assignment to " << tvm->object.name << '.' << tvm->var.name << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() assignment to:"));
	DBG(pgm.cc.comment(tvm->var.name.c_str()));
	DBG(pgm.cc.comment("TokenAssign::compile() regp = tvl->getreg(pgm)"));
	tvl = dynamic_cast<TokenVar *>(tvm);
	regp = &tvm->getreg(pgm);
	tdot = (TokenDot *)1;
    }
#endif
#if 1
    else
    if ( left->id() == TokenID::tkDot && ((TokenDot *)left)->left
    &&  ((TokenDot *)left)->left->type() == TokenType::ttVariable
    &&  ((TokenDot *)left)->right && ((TokenDot *)left)->right->type() == TokenType::ttIdentifier )
    {
	tdot = (TokenDot *)left;
	TokenIdent *tidn = ((TokenIdent *)tdot->right);
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
#endif
    else
    {
	throw "Assignment on a non-variable lval";
    }

    regdp.second = ltype; // set type

    // get left register
    x86::Gp &lreg = *regp;
    regdp.first = &lreg;
    /* x86::Gp &rreg =*/ right->compile(pgm, regdp);

    if ( !regdp.second )
	throw "TokenAssign: no rval type";
    if (  ltype->is_numeric() && !regdp.second->is_numeric() )
	throw "Expecting rval to be numeric";
    if ( !ltype->is_integer() &&  regdp.second->is_integer() )
	throw "Not expecting rval to be numeric";
    if ( ltype->is_string() && !regdp.second->is_string() )
	throw "Expecting rval to be string";

    if ( ltype->is_integer() )
    {
	DBG(cout << "TokenAssign::compile() integer to integer" << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() integer to integer"));
	tvl->var.modified();
	tvl->putreg(pgm);
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
/*	DBG(pgm.cc.comment("string_assign"));
	FuncCallNode* call = pgm.cc.call(imm(string_assign), FuncSignatureT<void, const char*, const char *>(CallConv::kIdHost));
	call->setArg(0, lreg);
	call->setArg(1, rreg);
*/
	tvl->var.modified();
	tvl->putreg(pgm);
    }
    else
	throw "Unsupported assignment";

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

// double uses xmm, not GpReg
x86::Xmm &TokenReal::getxmm(Program &pgm)
{
    _xmm = pgm.cc.newXmm();
    pgm.cc.movsd(_xmm, _val);
    return _xmm;
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

// member needs special handling
x86::Gp &TokenMember::getreg(Program &pgm)
{
    DBG(pgm.cc.comment("TokenMember::getreg()"));
    x86::Gp &oreg = pgm.tkFunction->getvreg(pgm.cc, &object);
    _reg = var.type->newreg(pgm.cc, var.name.c_str());
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
    return _reg;
}

// member also needs to be able to write the register back to variable
void TokenMember::putreg(Program &pgm)
{
    DBG(pgm.cc.comment("TokenMember::putreg()"));
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

    DBG(cc.comment("TokenCpnd::getvreg() on"));
    DBG(cc.comment(var->name.c_str()));

    if ( (rmi=register_map.find(var)) != register_map.end() )
    {
	DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::getvreg(" << var->name << ") found" << std::endl);
	// copy global variable to register -- needs to happen every time we need to access a global
	if ( var->is_global() && var->data && !var->is_constant() )
	{
	    DBG(cc.comment("TokenCpnd::getvreg() variable found, var->is_global() && var->data && !var->is_constant()"));
	    movreg(cc, rmi->second, var);
        }
	return rmi->second;
    }

    DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::getvreg(" << var->name << ") building register" << std::endl);
    if ( (var->flags & vfSTACK) && !var->type->is_numeric() )
    {
	switch(var->type->type())
	{
	    case DataType::dtSTRING:
		{
		    x86::Mem stack = cc.newStack(sizeof(std::string), 4);
		    x86::Gp reg = cc.newIntPtr(var->name.c_str());
		    cc.lea(reg, stack);
		    DBG(std::cout << "TokenCpnd::getvreg(" << var->name << ") stack var calling string_construct[" << (uint64_t)string_construct << ']' << std::endl);
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
		throw "TokenCpnd()::getvreg() unsupported type on stack";
		
	} // switch
    }
    else
    {
	DBG(cc.comment("TokenCpnd::getvreg() calling var->type->newreg()"));
	register_map[var] = var->type->newreg(cc, var->name.c_str());

	if ( (rmi=register_map.find(var)) == register_map.end() )
	    throw "TokenCpnd::getvreg() failure";

	DBG(cc.comment("TokenCpnd::getvreg() variable reg init, calling movreg on"));
	DBG(cc.comment(var->name.c_str()));
	if ( !(var->flags & vfSTACK) )
	    movreg(cc, rmi->second, var); // first initialization of non-stack register (regset)
	else
	if ( !(var->flags & vfPARAM) )
	// if it's a numeric stack register, we set it to zero, for the full size of the register
	// because subsequent operations (assignments, etc), may only access less significant
        // parts depending on the integer size, also, if we don't touch it here, we may not keep
        // access to this specific register for this variable
        {
	    if ( var->type->is_integer() )
		cc.xor_(rmi->second.r64(), rmi->second.r64());
	    else
	    if ( var->type->is_real() )
		cerr << "WARNING: floating point not handled by getvreg()" << endl;
	}
    }
    var->flags |= vfREGSET;

    if ( rmi == register_map.end() && (rmi=register_map.find(var)) == register_map.end() )
	throw "TokenCpnd::getvreg() failure";
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

void TokenOperator::setregdp(Program &pgm, regdefp_t &regdp)
{
    if ( left->type() == TokenType::ttReal || right->type() == TokenType::ttReal )
    {
	if ( !regdp.second )
	    regdp.second = &ddDOUBLE;
	if ( regdp.first )
	    return;
	_xmm = pgm.cc.newXmm("_xmm");
	regdp.first = &_xmm;
	return;
    }
    if ( left->type() == TokenType::ttInteger || right->type() == TokenType::ttInteger )
    {
	if ( !regdp.second )
	    regdp.second = &ddINT;
	if ( regdp.first )
	    return;
	_reg = pgm.cc.newGpq("_reg");
	regdp.first = &_reg;
	return;
    }
}

// add two integers
Operand &TokenAdd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAdd::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "+ missing lval operand"; }
    if ( !right ) { throw "+ missing rval operand"; }
    setregdp(pgm, regdp);
    if ( left->type() == TokenType::ttInteger )
    {
	if ( right->type() == TokenType::ttInteger )
	{
	    pgm.safemov(*regdp.first, left->val() + right->val());
	    return *regdp.first;
	}
	pgm.safemov(*regdp.first, left->val());
	Operand &lval = *regdp.first;
	regdp.first = NULL;
	Operand &rval = right->compile(pgm, regdp);
	pgm.safeadd(lval, rval);
	regdp.first = &lval;
	return *regdp.first;
    }
    if ( right->type() == TokenType::ttInteger )
    {
	pgm.safemov(*regdp.first, right->val());
	Operand &lval = *regdp.first;
	regdp.first = NULL;
	Operand &rval = left->compile(pgm, regdp);
	pgm.safeadd(lval, rval);
	regdp.first = &lval;
	return *regdp.first;
    }
    DBG(cout << "TokenAdd::compile() left->compile()" << endl);
    Operand &lval = left->compile(pgm, regdp); // get lval into register
    if ( !regdp.second ) { throw "TokenAdd::compile() left->compile didn't set datatype"; }
    _reg = regdp.second->newreg(pgm.cc, "TokenAdd._reg"); // use tmp for right side
    regdp.first = &_reg;
    DBG(cout << "TokenAdd::compile() right->compile()" << endl);
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeadd(lval, rval);
    regdp.first = &lval;
    return *regdp.first;
}

// subtract two integers
Operand &TokenSub::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenSub::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "- missing lval operand"; }
    if ( !right ) { throw "- missing rval operand"; }
//  if ( !regdp.first ) { throw "- missing register"; }
    DBG(cout << "TokenSub::compile() left->compile()" << endl);
    Operand &lval = left->compile(pgm, regdp); // get lval into register
    if ( !regdp.second ) { throw "TokenSub::compile() left->compile didn't set datatype"; }
    _reg = regdp.second->newreg(pgm.cc, "TokenSub._reg"); // use tmp for right side
    regdp.first = &_reg;
    DBG(cout << "TokenSub::compile() right->compile()" << endl);
    Operand &rval = right->compile(pgm, regdp);
    pgm.safesub(lval, rval);
    regdp.first = &lval;
    return *regdp.first;
}

// make number negative
Operand &TokenNeg::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenNeg::Compile() TOP" << endl);
    if ( !right ) { throw "- missing rval operand"; }
//  if ( !regdp.first )
    /*Operand &rval =*/ right->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenNeg::compile() right->compile didn't set datatype"; }
    DBG(pgm.cc.comment("TokenNeg::compile() pgm.cc.neg(regdp.first)"));
    pgm.safeneg(*regdp.first);
    return *regdp.first;
}

// multiply two integers
Operand &TokenMul::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMul::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "* missing lval operand"; }
    if ( !right ) { throw "* missing rval operand"; }
//  if ( !regdp.first ) { throw "* missing register"; }
    DBG(cout << "TokenMul::compile() left->compile()" << endl);
    Operand &lval = left->compile(pgm, regdp); // get lval into register
    if ( !regdp.second ) { throw "TokenMul::compile() left->compile didn't set datatype"; }
    _reg = regdp.second->newreg(pgm.cc, "TokenMul._reg"); // use tmp for right side
    regdp.first = &_reg;
    DBG(cout << "TokenMul::compile() right->compile()" << endl);
    Operand &rval = right->compile(pgm, regdp);
    pgm.safemul(lval, rval);
    regdp.first = &lval;
    return *regdp.first;
}

// divide two integers
Operand &TokenDiv::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDiv::Compile() TOP" << endl);
    if ( !left )  { throw "/ missing lval operand"; } 
    if ( !right ) { throw "/ missing rval operand"; }
//  if ( !regdp.first ) { throw "/ missing register"; }
    Operand remainder = pgm.cc.newInt64("TokenDiv::remainder");
    Operand &dividend = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenDiv::compile() left->compile didn't set datatype"; }
    _reg = regdp.second->newreg(pgm.cc, "divisor"); // use tmp for right side
    regdp.first = &_reg;
    DBG(cout << "TokenDiv::compile() right->compile()" << endl);
    Operand &divisor = right->compile(pgm, regdp);
    pgm.safexor(remainder, remainder);
    DBG(pgm.cc.comment("TokenDiv::compile() pgm.cc.div(remainder, _reg, rval)"));
    pgm.safediv(remainder, dividend, divisor);
    regdp.first = &dividend;
    return *regdp.first;
}

// modulus
Operand &TokenMod::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMod::Compile() TOP" << endl);
    if ( !left )  { throw "% missing lval operand"; }
    if ( !right ) { throw "% missing rval operand"; }
    if ( !regdp.first ) // { throw "% missing register"; }
    {
	_reg = pgm.cc.newInt64("remainder");
	regdp.first = &_reg;
    }
    Operand &remainder = *regdp.first;
    Operand _dividend;
    if ( regdp.second )
	_dividend = regdp.second->newreg(pgm.cc, "dividend");
    else
    {
	_dividend = pgm.cc.newInt64("dividend");
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

    // hard coding some basic ostream support for now, will use operator overloading later
    if ( left->type() == TokenType::ttVariable && dynamic_cast<TokenVar *>(left)->var.type->has_ostream() )
    {
	TokenVar *tvl = dynamic_cast<TokenVar *>(left);
	DBG(pgm.cc.comment("TokenBSL::compile() (ostream &)tvl->getreg(pgm)"));
	x86::Gp &lval = tvl->getreg(pgm); // get ostream register

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
	regdp.objreg = &lval;
	Operand &rval = right->compile(pgm, regdp); // compile right side

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
		case DataType::dtFLOAT: call = pgm.cc.call(imm(streamout_numeric<float>),  FuncSignatureT<void, void *, float>(CallConv::kIdHost));	break;
		case DataType::dtDOUBLE:call = pgm.cc.call(imm(streamout_numeric<double>), FuncSignatureT<void, void *, double>(CallConv::kIdHost));	break;
		default: throw "TokenBSL::compile() unsupported numeric type";
	    }
	    call->setArg(0, lval);
#if 1
	    if ( regdp.first->isReg() )
	    {
		if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupVec) )
		    call->setArg(1, regdp.first->as<x86::Xmm>());
		else
		if ( regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupGp) )
		    call->setArg(1, regdp.first->as<x86::Gp>());
		else
		    throw "TokenBSL::compile() unexpected parameter Operand";
	    }
	    else
	    if ( regdp.first->isImm() )
		call->setArg(1, regdp.first->as<Imm>());
#else
	    if ( regdp.second->is_real() && regdp.xmm )
		call->setArg(1, *regdp.xmm);
	    else
		call->setArg(1, rval);
#endif
	}
	else
	if ( regdp.second->is_string() )
	{
	    if ( !regdp.first->isReg() ) { throw "TokenBSL::compile() regdp.first->isReg() is FALSE"; }
	    if ( !regdp.first->as<BaseReg>().isGroup(BaseReg::kGroupGp) ) { throw "TokenBSL::compile() regdp.first not GpReg"; }
	    DBG(cout << "TokenBSL::compile() regdp.second->is_string()" << endl);
	    DBG(pgm.cc.comment("TokenBSL::compile() regdp.second->is_string()"));
	    DBG(pgm.cc.comment("pgm.cc.call(streamout_string)"));
	    FuncCallNode* call = pgm.cc.call(imm(streamout_string), FuncSignatureT<void, void *, void *>(CallConv::kIdHost));
	    DBG(pgm.cc.comment("call->setArg(0, lval)"));
	    call->setArg(0, lval);
	    DBG(pgm.cc.comment("call->setArg(1, rval"));
	    call->setArg(1, regdp.first->as<x86::Gp>());
	}
	else
	{
	    cerr << "TokenBSL::compile() regdp.second.name: " << regdp.second->name << " regdp.second->type() " << (int)regdp.second->type() << endl;
	    throw "TokenBSL::compile unsupported dataype";
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

    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBSL::compile() left->compile didn't set datatype"; }
    _reg = regdp.second->newreg(pgm.cc, "TokenBSL._reg"); // use tmp for right side
    regdp.first = &_reg;
    DBG(cout << "TokenBSL::compile() right->compile()" << endl);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBSL::compile() pgm.cc.shl(lval, rval.r8())"));
    pgm.safeshl(lval, rval);

    DBG(cout << "TokenBSL::Compile() END" << endl);
    regdp.first = &lval;
    return *regdp.first;
}

// bit shift right
Operand &TokenBSR::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBSR::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBSR::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBSR::compile() pgm.cc.shr(_reg, rreg.r8())"));
    pgm.safeshr(_reg, rval);

    return _reg;
}

// bitwise or |
Operand &TokenBor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBor::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBor::compile() pgm.cc.or_(_reg, rval)"));
    pgm.safeor(_reg, rval);

    return _reg;
}

// bitwise xor ^
Operand &TokenXor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenXor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenXor::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenXor::compile() pgm.cc.xor_(_reg, rval)"));
    pgm.safexor(_reg, rval);

    return _reg;
}

// bitwise and &
Operand &TokenBand::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenBand::compile() pgm.safemov(_reg, lval)"));
    _reg = pgm.cc.newGpq();
    pgm.safemov(_reg, lval);
    DBG(pgm.cc.comment("TokenBand::compile() pgm.cc.and_(_reg, rval)"));
    pgm.safeand(_reg, rval);

    return _reg;
}


// bitwise not ~
Operand &TokenBnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBnot::Compile() TOP" << endl);
    if ( left )   { throw "Bitwise not has lval!"; }
    if ( !right ) { throw "~ missing rval operand"; }
//  if ( !regdp.first ) { throw "~ missing register"; }
    /*Operand &rval =*/ right->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBnot::compile() right->compile didn't set datatype"; }
    DBG(pgm.cc.comment("TokenBnot::compile() pgm.cc.not_(regdp.first)"));
    pgm.safenot(*regdp.first);
    return *regdp.first;
}

/////////////////////////////////////////////////////////////////////////////
// logic operators                                                         //
/////////////////////////////////////////////////////////////////////////////

// logical not !
Operand &TokenLnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLnot::Compile() TOP" << endl);
    if ( left )   { throw "! unexpected lval!"; }
    if ( !right ) { throw "! missing rval operand"; }
//  if ( !regdp.first ) { throw "! missing register"; }
    Operand &rval = right->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenLnot::compile() right->compile didn't set datatype"; }
    DBG(pgm.cc.comment("TokenLnot::compile() pgm.cc.sete(regdp.first)"));
    pgm.safetest(rval, rval); // test rval is 0
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
    if ( !left )  { throw "|| missing lval operand"; }
    if ( !right ) { throw "|| missing rval operand"; }
    Label done = pgm.cc.newLabel();	// label to skip further tests
    x86::Gp &reg  = getreg(pgm);	// get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLor::compile() pgm.cc.test(lval, lval)"));
    pgm.safetest(lval, lval);		// test lval is 0
    DBG(pgm.cc.comment("TokenLor::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8()); 		// if lval != 0, ret = 1
    pgm.cc.jne(done);			// if lval != 0, jump to done
    pgm.safetest(rval, rval);		// test rval is 0
    DBG(pgm.cc.comment("TokenLor::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8());		// if rval != 0, ret = 1
    pgm.cc.bind(done);			// done is here
    return reg;				// return register
}

// logical and &&
//
// Pseudocode: if (!lval) return 0;  if (!rval) return 0;  return 1;
//
Operand &TokenLand::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    Label done = pgm.cc.newLabel();	// label to skip further tests
    x86::Gp &reg  = getreg(pgm);	// get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLand::compile() pgm.cc.test(lval, lval)"));
    pgm.safetest(lval, lval);		// test lval is 0
    DBG(pgm.cc.comment("TokenLand::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8()); 		// if lval != 0, ret = 1
    pgm.cc.je(done);			// if lval == 0, jump to done
    pgm.safetest(rval, rval);		// test rval is 0
    DBG(pgm.cc.comment("TokenLand::compile() pgm.cc.sete(_reg)"));
    pgm.cc.setne(reg.r8());		// if rval != 0, ret = 1
    pgm.cc.bind(done);			// done is here
    return reg;				// return register
}


/////////////////////////////////////////////////////////////////////////////
// comparison operators                                                    //
/////////////////////////////////////////////////////////////////////////////


// Equal to: ==
Operand &TokenEquals::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenEquals::Compile() TOP" << endl);
    if ( !left )  { throw "= missing lval operand"; }
    if ( !right ) { throw "= missing rval operand"; } 
    x86::Gp &reg  = getreg(pgm); // get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(cout << "TokenEquals: lval.size() " << lval.size() << " rval.size() " << rval.size() << endl);
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenEquals::compile() pgm.cc.sete(reg)"));
    pgm.cc.sete(reg.r8());
    return reg;
}

// Not equal to: !=
Operand &TokenNotEq::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenNotEq::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenNotEq::compile() pgm.cc.setne(reg)"));
    pgm.cc.setne(reg.r8());
    return reg;
}

// Less than: <
Operand &TokenLT::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLT::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    DBG(pgm.cc.comment("TokenLT::compile() reg = getreg(pgm)"));
    x86::Gp &reg  = getreg(pgm); // get clean register
    DBG(pgm.cc.comment("TokenLT::compile() lval = left->compile(pgm, regdp)"));
    Operand &lval = left->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLT::compile() rval = right->compile(pgm, regdp)"));
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenLT::compile() pgm.cc.setl(reg)"));
    pgm.cc.setl(reg.r8());
    return reg;
}

// Less than or equal to: <=
Operand &TokenLE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenLE::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenLE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenLE::compile() pgm.cc.setle(_reg)"));
    pgm.cc.setle(reg.r8());
    return reg;
}

// Greater than: >
Operand &TokenGT::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenGT::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenGT::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenGT::compile() pgm.cc.setg(reg)"));
    pgm.cc.setg(reg.r8());
    return reg;
}

// Greater than or equal to: >=
Operand &TokenGE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenGE::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
    DBG(pgm.cc.comment("TokenGE::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval);
    DBG(pgm.cc.comment("TokenGE::compile() pgm.cc.setge(reg)"));
    pgm.cc.setge(reg.r8());
    return reg;
}


// Greater than gives 1, less than gives -1, equal to gives 0 (<=>)
Operand &Token3Way::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "Token3Way::Compile() TOP" << endl);
    Label done = pgm.cc.newLabel();	// label to skip further tests
    Label sign = pgm.cc.newLabel();	// label to negate _reg (make negative)
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    x86::Gp &reg  = getreg(pgm); // get clean register
    Operand &lval = left->compile(pgm, regdp);
    Operand &rval = right->compile(pgm, regdp);
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
Operand &TokenDot::compile(Program &pgm, regdefp_t &regdp)
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
Operand &TokenVar::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenVar::compile() reg = getreg()"));
    x86::Gp &reg = getreg(pgm);

    if ( !regdp.second )
	regdp.second = _datatype;

    if ( regdp.first )
    {
	if ( !reg.isEqual(*regdp.first) && regdp.first != &reg )
	{
	    DBG(pgm.cc.comment("TokenVar::compile() safemov(*ret, reg)"));
	    pgm.safemov(*regdp.first, reg);
	}
	return *regdp.first;
    }

    regdp.first = &reg;
    return reg;
}

// load variable into register
Operand &TokenMember::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenMember::compile() reg = getreg()"));
    x86::Gp &reg = getreg(pgm);

    if ( !regdp.second )
	regdp.second = _datatype;

    if ( regdp.first )
    {
	DBG(pgm.cc.comment("TokenMember::compile() safemov(*ret, reg)"));
	pgm.safemov(*regdp.first, reg);
	return *regdp.first;
    }

    return reg;
}

// load double into register
Operand &TokenReal::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !regdp.second )
    {
	if ( !_datatype ) { throw "TokenReal has NULL _datatype"; }
	regdp.second = _datatype;
	DBG(pgm.cc.comment("TokenReal::compile() setting _datatype to double"));
    }
    if ( regdp.first )
    {
	pgm.safemov(*regdp.first, _val);
	return _reg;
    }
    _xmm = pgm.cc.newXmm();
    pgm.cc.movsd(_xmm, _val);
    regdp.first = &_xmm;
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
	pgm.safemov(*regdp.first, _token);
	return *regdp.first;
    }
    DBG(cout << "TokenInt::compile[" << (uint64_t)this << "]() value: " << (int)_token << endl);
    regdp.first = &_reg;
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
    DBG(pgm.cc.comment("TokenIF::compile() pgm.cc.test(reg, reg)"));
    pgm.safetest(reg, reg);			// compare to zero
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
    pgm.safetest(reg, reg);		// compare to zero
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
    Operand &reg = condition->compile(pgm, regdp);// get condition result
    pgm.safetest(reg, reg);			// compare to zero
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
    pgm.safetest(reg, reg);			// compare to zero
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
