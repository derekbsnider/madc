///////////////////////////////////////////////////////////////////////////
//									 //
// madc "typesafe" methods to make it easier to deal with register types //
//									 //
///////////////////////////////////////////////////////////////////////////
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

// simple for now, should have different versions for signed vs unsigned
// small to big vs big to small, etc, as we need to ensure that moving
// small to big doesn't leave unwanted data in the other part of the register
void Program::safemov(x86::Gp &r1, x86::Gp &r2)
{
    DBG(cc.comment("safemov(Gp, Gp)"));
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
    switch(r1.type())
    {
	case BaseReg::kTypeGp8Lo: cc.cvtsd2si(r1.r32(), r2);	break;
	case BaseReg::kTypeGp8Hi: cc.cvtsd2si(r1.r32(), r2);	break;
	case BaseReg::kTypeGp32:  cc.cvtsd2si(r1, r2);		break;
	case BaseReg::kTypeGp64:  cc.cvtsd2si(r1, r2);		break;
	default: throw "Program::safemov() cannot match register types";
    }
}
void Program::safemov(x86::Xmm &r1, x86::Gp &r2)
{
   throw "safemov() unable to move gp to xmm";
}
void Program::safemov(x86::Xmm &r1, x86::Xmm &r2)
{
   DBG(cc.comment("safemov(Xmm, Xmm)"));
   cc.movsd(r1, r2);
}
void Program::safemov(x86::Xmm &r1, x86::Mem &r2)
{
   DBG(cc.comment("safemov(Xmm, Mem)"));
   cc.movsd(r1, r2);
}
void Program::safemov(x86::Gp &r1, x86::Mem &r2)
{
   DBG(cc.comment("safemov(Gp, Mem)"));
   cc.mov(r1, r2);
}
void Program::safemov(x86::Xmm &r1, Imm &r2)
{
   throw "safemov() unable to move imm to xmm";
}

void Program::safemov(Operand &op1, int i)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	x86::Mem _const = cc.newDoubleConst(ConstPool::kScopeLocal, (double)i);
	DBG(cc.comment("safemov(Xmm, ConstPool)"));
	cc.movsd(op1.as<x86::Xmm>(), _const);
	return;
    }
    DBG(cc.comment("safemov(Operand, int)"));
    Operand op2 = imm(i);
    safemov(op1, op2);
}

void Program::safemov(Operand &op1, double d)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	x86::Mem _const = cc.newDoubleConst(ConstPool::kScopeLocal, d);
	DBG(cc.comment("safemov(Xmm, ConstPool)"));
	cc.movsd(op1.as<x86::Xmm>(), _const);
	return;
    }
    DBG(cc.comment("safemov(Operand, (int)double)"));
    Operand op2 = imm((int)d);
    safemov(op1, op2);
}

// should handle all necessary conversions...
void Program::safemov(Operand &op1, Operand &op2)
{
    DBG(cc.comment("safemov(Operand, Operand)"));
    if ( !op1.isReg() ) { throw "safemov() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() && !op2.isMem() ) { throw "safemov() rval is not register, memory, or immediate"; }
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
    {
	DBG(cc.comment("safemov(Operand=Xmm, Operand)"));
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Gp>());
	else
	if ( op2.isMem() )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Mem>());
	else
	if ( op2.isImm() )
	    safemov(op1.as<x86::Xmm>(), op2.as<Imm>());
	else
	    throw "safemov() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
    {
	DBG(cc.comment("safemov(Operand=Gp, Operand)"));
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupVec) )
	    safemov(op1.as<x86::Gp>(), op2.as<x86::Xmm>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	    safemov(op1.as<x86::Gp>(), op2.as<x86::Gp>());
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
    if ( !op1.isReg() ) { cerr << op1.opType() << endl; throw "safeadd() lval is not a register"; }
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
//	x86::Xmm tmp = cc.newXmm();
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

#if 0
void printint(int i)
{
    cout << "printint: " << i << endl;
}
#endif

// perform cc.div with size casting
void Program::safediv(Operand &op1, Operand &op2, Operand &op3)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safediv() left operand is not a Gp register";
    if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safediv() middle operand is not a Gp register";
    if ( !op3.isReg() || !op3.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safediv() right operand is not a Gp register";
#if 0
    FuncCallNode* call;
    call = cc.call(imm(printint), FuncSignatureT<void, int>(CallConv::kIdHost));
    call->setArg(0, op1.as<x86::Gp>());
    call = cc.call(imm(printint), FuncSignatureT<void, int>(CallConv::kIdHost));
    call->setArg(0, op2.as<x86::Gp>());
    call = cc.call(imm(printint), FuncSignatureT<void, int>(CallConv::kIdHost));
    call->setArg(0, op3.as<x86::Gp>());
#endif
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
    {
	cc.comment("cc.xor_(gp, imm)");
	cc.xor_(op1.as<x86::Gp>(), op2.as<Imm>());
    }
    else
    {
	cc.comment("cc.xor_(gp, gp)");
	cc.xor_(op1.as<x86::Gp>(), op2.as<x86::Gp>());
    }
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
    if ( op.isImm() )
    {
	x86::Gp reg = cc.newGpq();
	cc.mov(reg, op.as<Imm>().i64());
	cc.ret(reg);
    }
    else
	throw "saferet() unsupported operand";
}



// perform a test on two operands
void Program::safetest(Operand &op1, Operand &op2)
{
    DBG(cout << "Program::safetest(" << op1.opType() << ", " << op2.opType() << ')' << endl);
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safetest(op1, op2) left operand is not a Gp register";
    if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(BaseReg::kGroupGp) )
	throw "safetest(op1, op2) right operand is not a Gp register";
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
