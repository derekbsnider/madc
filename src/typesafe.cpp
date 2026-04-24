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
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;
using namespace asmjit;

// simple for now, should have different versions for signed vs unsigned
// small to big vs big to small, etc, as we need to ensure that moving
// small to big doesn't leave unwanted data in the other part of the register
void Program::safemov(x86::Gp &r1, x86::Gp &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Gp, Gp)"));
    uint32_t rs = r1.x86RmSize();
    uint32_t ms = r2.x86RmSize();
    if ( rs > ms )
    {
	// Dest is wider than source — sign- or zero-extend based on the
	// source type's signedness.
	bool is_unsigned = d2 && d2->is_unsigned();
	if ( ms == 4 )
	{
	    if ( is_unsigned )
		cc.mov(r1.r32(), r2);    // implicit zero-extend to r64
	    else
		cc.movsxd(r1, r2);       // sign-extend 32→64
	}
	else if ( ms == 2 || ms == 1 )
	{
	    if ( rs >= 8 )
	    {
		if ( is_unsigned ) cc.movzx(r1.r32(), r2);
		else               cc.movsx(r1.r32(), r2);
	    }
	    else
	    {
		if ( is_unsigned ) cc.movzx(r1, r2);
		else               cc.movsx(r1, r2);
	    }
	}
	else
	    cc.mov(r1, r2);
    }
    else
    switch(r1.type())
    {
	case RegType::kGp8Lo: cc.mov(r1, r2.r8Lo());  break;
	case RegType::kGp8Hi: cc.mov(r1, r2.r8Hi());  break;
	case RegType::kGp16:  cc.mov(r1, r2.r16());   break;
	case RegType::kGp32:  cc.mov(r1, r2.r32());   break;
	case RegType::kGp64:  cc.mov(r1, r2.r64());   break;
	default: throw "Program::safemov() cannot match register types";
    }
}

void Program::safemov(x86::Gp &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Gp, Xmm)"));
    if ( d2 && d2->size == sizeof(float) )
    switch(r1.type())
    {
	case RegType::kGp8Lo: cc.cvtss2si(r1.r32(), r2);	break;
	case RegType::kGp8Hi: cc.cvtss2si(r1.r32(), r2);	break;
	case RegType::kGp32:  cc.cvtss2si(r1, r2);		break;
	case RegType::kGp64:  cc.cvtss2si(r1, r2);		break;
	default: throw "Program::safemov() cannot match register types";
    }
    else
    switch(r1.type())
    {
	case RegType::kGp8Lo: cc.cvtsd2si(r1.r32(), r2);	break;
	case RegType::kGp8Hi: cc.cvtsd2si(r1.r32(), r2);	break;
	case RegType::kGp32:  cc.cvtsd2si(r1, r2);		break;
	case RegType::kGp64:  cc.cvtsd2si(r1, r2);		break;
	default: throw "Program::safemov() cannot match register types";
    }
}
void Program::safemov(x86::Xmm &r1, x86::Gp &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Xmm, Gp)"));
    if ( d1 && d1->size == sizeof(float) )
	cc.cvtsi2ss(r1, r2);
    else
	cc.cvtsi2sd(r1, r2);
}
void Program::safemov(x86::Xmm &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Xmm, Xmm)"));
    if ( d1 && d1->size == sizeof(float) )
    {
	if ( d2 && d2->size == sizeof(float) )
	    cc.movss(r1, r2);
	else
	    cc.cvtsd2ss(r1, r2);
    }
    else
    {
	if ( d2 && d2->size == sizeof(float) )
	    cc.cvtss2sd(r1, r2);
	else
	    cc.movsd(r1, r2);
    }
}
void Program::safemov(x86::Xmm &r1, x86::Mem &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Xmm, Mem)"));
    // When d2 isn't supplied, fall back to the Mem's actual size so a
    // 3-arg safemov that just says "destination is float" doesn't
    // misread a 4-byte Mem as a double (the old default blindly used
    // cvtsd2ss, reading 8 bytes from a 4-byte slot and producing
    // garbage).
    bool dst_is_float = d1 && d1->size == sizeof(float);
    bool src_is_float = d2 ? (d2->size == sizeof(float))
			   : (r2.size() == sizeof(float));
    if ( dst_is_float )
    {
	if ( src_is_float )
	    cc.movss(r1, r2);
	else
	    cc.cvtsd2ss(r1, r2);
    }
    else
    {
	if ( src_is_float )
	    cc.cvtss2sd(r1, r2);
	else
	    cc.movsd(r1, r2);
    }
}
void Program::safemov(x86::Gp &r1, x86::Mem &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Gp, Mem)"));
    uint32_t rs = r1.x86RmSize();
    uint32_t ms = r2.x86RmSize();
    if ( rs > ms && ms > 0 )
    {
	// Dest is wider than source Mem — extend to fill.
	bool is_unsigned = d2 && d2->is_unsigned();
	if ( ms == 4 )
	{
	    if ( is_unsigned )
		cc.mov(r1.r32(), r2);    // implicit zero-extend to r64
	    else
		cc.movsxd(r1, r2);       // sign-extend 32→64
	}
	else if ( ms == 2 )
	{
	    if ( rs >= 8 )
	    {
		if ( is_unsigned ) cc.movzx(r1.r32(), r2);
		else               cc.movsx(r1.r32(), r2);
	    }
	    else
	    {
		if ( is_unsigned ) cc.movzx(r1, r2);
		else               cc.movsx(r1, r2);
	    }
	}
	else if ( ms == 1 )
	{
	    if ( rs >= 8 )
	    {
		if ( is_unsigned ) cc.movzx(r1.r32(), r2);
		else               cc.movsx(r1.r32(), r2);
	    }
	    else
	    {
		if ( is_unsigned ) cc.movzx(r1, r2);
		else               cc.movsx(r1, r2);
	    }
	}
	else
	    cc.mov(r1, r2);
    }
    else
    {
	cc.mov(r1, r2);
    }
}

void Program::safemov(x86::Xmm &r1, Imm &r2, DataDef *d1, DataDef *d2)
{
    throw "safemov() unable to move imm to xmm";
}

void Program::safemov(Operand &op1, int i, DataDef *d1, DataDef *d2)
{
    safemov(op1, (int64_t)i, d1, d2);
}

void Program::safemov(Operand &op1, int64_t i, DataDef *d1, DataDef *d2)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	x86::Mem _const = cc.newDoubleConst(ConstPoolScope::kLocal, (double)i);
	DBG(cc.comment("safemov(Xmm, ConstPool)"));
	if ( d1 && d1->size == sizeof(float) )
	    cc.cvtsd2ss(op1.as<x86::Xmm>(), _const);
	else
	    cc.movsd(op1.as<x86::Xmm>(), _const);
	return;
    }
    DBG(cc.comment("safemov(Operand, int64_t)"));
    Operand op2 = imm(i);
    safemov(op1, op2, d1, d2);
}

void Program::safemov(Operand &op1, double d, DataDef *d1, DataDef *d2)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	x86::Mem _const = cc.newDoubleConst(ConstPoolScope::kLocal, d);
	DBG(cc.comment("safemov(Xmm, ConstPool)"));
	if ( d1 && d1->size == sizeof(float) )
	    cc.cvtsd2ss(op1.as<x86::Xmm>(), _const);
	else
	    cc.movsd(op1.as<x86::Xmm>(), _const);
	return;
    }
    DBG(cc.comment("safemov(Operand, (int)double)"));
    Operand op2 = imm((int)d);
    safemov(op1, op2, d1, d2);
}


void Program::safemov(x86::Mem &m, x86::Gp &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Mem, Gp)"));
    // Pick the register view that matches the Mem's size — an `mov word
    // ptr, r64` is not a valid x86 encoding and asmjit silently drops it
    // (or emits a truncated op). Without this, callers that widen a member
    // to Gp64 for arithmetic would fail to write back to the narrower Mem.
    uint32_t msz = m.size();
    if ( !msz ) msz = (uint32_t)r2.size();
    switch(msz)
    {
	case 1: cc.mov(m, r2.r8());   break;
	case 2: cc.mov(m, r2.r16());  break;
	case 4: cc.mov(m, r2.r32());  break;
	case 8: cc.mov(m, r2.r64());  break;
	default: throw "Program::safemov(Mem, Gp) unsupported Mem size";
    }
}

void Program::safemov(x86::Mem &m, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Mem, Xmm)"));
    // Destination is a float (4 byte) or double (8 byte) slot. The
    // source Xmm holds the value in whichever precision produced it —
    // if the destination is narrower we must cvtsd2ss down to float
    // first (movss would otherwise store the raw low 32 bits, which
    // for a double-precision value are the low mantissa bits, not a
    // valid float32).
    uint32_t ms = m.size();
    if ( !ms && d1 ) ms = (uint32_t)d1->size;
    bool dst_is_float = (ms == 4) || (d1 && d1->size == sizeof(float));
    bool src_is_float = d2 && d2->size == sizeof(float);
    if ( dst_is_float )
    {
	if ( !src_is_float )
	{
	    x86::Xmm tmp = cc.newXmm("_fx_tmp");
	    cc.cvtsd2ss(tmp, r2);
	    cc.movss(m, tmp);
	}
	else
	    cc.movss(m, r2);
    }
    else
    {
	if ( src_is_float )
	{
	    x86::Xmm tmp = cc.newXmm("_fx_tmp");
	    cc.cvtss2sd(tmp, r2);
	    cc.movsd(m, tmp);
	}
	else
	    cc.movsd(m, r2);
    }
}

// should handle all necessary conversions...
void Program::safemov(Operand &op1, Operand &op2, DataDef *d1, DataDef *d2)
{
    DBG(cc.comment("safemov(Operand, Operand)"));
    if ( op1.isMem() )
    {
	DBG(cc.comment("safemov(Operand=Mem, Operand)"));
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safemov(op1.as<x86::Mem>(), op2.as<x86::Xmm>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safemov(op1.as<x86::Mem>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isImm() )
	{
	    // x86's `mov m64, imm` only carries a 32-bit sign-extended
	    // immediate. For int64 literals that don't fit in int32
	    // (`long long big = 9000000000;`), we'd silently store the
	    // truncated lower 32 bits. Bounce through a register for
	    // out-of-range literals.
	    int64_t iv = op2.as<Imm>().value();
	    uint32_t msz = op1.as<x86::Mem>().size();
	    bool too_big = (msz == 8 && (iv < INT32_MIN || iv > INT32_MAX));
	    if ( too_big )
	    {
		x86::Gp tmp = cc.newGpq("_imm64_tmp");
		cc.mov(tmp, iv);
		cc.mov(op1.as<x86::Mem>(), tmp);
	    }
	    else
		cc.mov(op1.as<x86::Mem>(), op2.as<Imm>());
	}
	else
	if ( op2.isMem() )
	{
	    // Mem <- Mem: bounce through a typed temporary so 4-byte stack
	    // slots don't get widened to accidental 8-byte loads/stores.
	    DataDef *tmp_type = d1 ? d1 : d2;
	    if ( tmp_type && tmp_type->is_real() )
	    {
		x86::Xmm tmp = cc.newXmm("_tmp_mm");
		safemov(tmp, op2.as<x86::Mem>(), d1, d2);
		if ( op1.as<x86::Mem>().size() <= 4 )
		    cc.movss(op1.as<x86::Mem>(), tmp);
		else
		    cc.movsd(op1.as<x86::Mem>(), tmp);
	    }
	    else
	    {
		x86::Gp tmp = cc.newGpq("_tmp_mm");
		safemov(tmp, op2.as<x86::Mem>(), d1, d2);
		safemov(op1.as<x86::Mem>(), tmp, d1, d2);
	    }
	}
	else
	    throw "safemov() rval is unsupported";
	return;
    }
    if ( !op1.isReg() ) { throw "safemov() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() && !op2.isMem() ) { throw "safemov() rval is not register, memory, or immediate"; }
    if ( op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	DBG(cc.comment("safemov(Operand=Xmm, Operand)"));
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Xmm>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isMem() )
	    safemov(op1.as<x86::Xmm>(), op2.as<x86::Mem>(), d1, d2);
	else
	if ( op2.isImm() )
	    safemov(op1.as<x86::Xmm>(), op2.as<Imm>(), d1, d2);
	else
	    throw "safemov() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	DBG(cc.comment("safemov(Operand=Gp, Operand)"));
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safemov(op1.as<x86::Gp>(), op2.as<x86::Xmm>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safemov(op1.as<x86::Gp>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isMem() )
	{
	    // Forward to typed (Gp, Mem) so size-mismatched loads get sign/zero-extension.
	    x86::Mem mm = op2.as<x86::Mem>();
	    x86::Gp  gp = op1.as<x86::Gp>();
	    safemov(gp, mm, d1, d2);
	}
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
void Program::safeadd(x86::Gp &r1, x86::Gp &r2, DataDef *d1, DataDef *d2)
{
    switch(r1.type())
    {
	case RegType::kGp8Lo: cc.add(r1, r2.r8Lo());  break;
	case RegType::kGp8Hi: cc.add(r1, r2.r8Hi());  break;
	case RegType::kGp16:  cc.add(r1, r2.r16());   break;
	case RegType::kGp32:  cc.add(r1, r2.r32());   break;
	case RegType::kGp64:  cc.add(r1, r2.r64());   break;
	default: throw "Program::safeadd() cannot match register types";
    }
}

void Program::safeadd(x86::Gp &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    throw "safeadd() unable to add xmm to gp";
}
void Program::safeadd(x86::Xmm &r1, x86::Gp &r2, DataDef *d1, DataDef *d2)
{
    throw "safeadd() unable to add gp to xmm";
}
void Program::safeadd(x86::Xmm &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    if ( d1 && d1->size == sizeof(float) )
    {
	DBG(cc.comment("Program::safeadd(r1, r2, float)"));
	cc.addss(r1, r2);
    }
    else
    {
	DBG(cc.comment("Program::safeadd(r1, r2, double)"));
	cc.addsd(r1, r2);
    }
}
void Program::safeadd(x86::Xmm &r1, Imm &r2, DataDef *d1, DataDef *d2)
{
    throw "safeadd() unable to add imm to xmm";
}

void Program::safeadd(Operand &op1, int i, DataDef *d1, DataDef *d2)
{
    Operand op2 = imm(i);
    safeadd(op1, op2, d1, d2);
}

// should handle all necessary conversions...
void Program::safeadd(Operand &op1, Operand &op2, DataDef *d1, DataDef *d2)
{
    if ( !op1.isReg() ) { cerr << (uint32_t)op1.opType() << endl; throw "safeadd() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() ) { throw "safeadd() rval is not register or immediate"; }
    if ( op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safeadd(op1.as<x86::Xmm>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safeadd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>(), d1, d2);
	else
	if ( op2.isImm() )
	    safeadd(op1.as<x86::Xmm>(), op2.as<Imm>(), d1, d2);
	else
	    throw "safeadd() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safeadd(op1.as<x86::Gp>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safeadd(op1.as<x86::Gp>(), op2.as<x86::Xmm>(), d1, d2);
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
void Program::safesub(x86::Gp &r1, x86::Gp &r2, DataDef *d1, DataDef *d2)
{
    switch(r1.type())
    {
	case RegType::kGp8Lo: cc.sub(r1, r2.r8Lo());  break;
	case RegType::kGp8Hi: cc.sub(r1, r2.r8Hi());  break;
	case RegType::kGp16:  cc.sub(r1, r2.r16());   break;
	case RegType::kGp32:  cc.sub(r1, r2.r32());   break;
	case RegType::kGp64:  cc.sub(r1, r2.r64());   break;
	default: throw "Program::safesub() cannot match register types";
    }
}

void Program::safesub(x86::Gp &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    throw "safesub() unable to sub xmm to gp";
}
void Program::safesub(x86::Xmm &r1, x86::Gp &r2, DataDef *d1, DataDef *d2)
{
    throw "safesub() unable to sub gp to xmm";
}
void Program::safesub(x86::Xmm &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    if ( d1 && d1->size == sizeof(float) )
	cc.subss(r1, r2);
    else
	cc.subsd(r1, r2);
}
void Program::safesub(x86::Xmm &r1, Imm &r2, DataDef *d1, DataDef *d2)
{
    throw "safesub() unable to sub imm to xmm";
}

void Program::safesub(Operand &op1, int i, DataDef *d1, DataDef *d2)
{
    Operand op2 = imm(i);
    safesub(op1, op2);
}

// should handle all necessary conversions...
void Program::safesub(Operand &op1, Operand &op2, DataDef *d1, DataDef *d2)
{
    if ( !op1.isReg() ) { throw "safesub() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() ) { throw "safesub() rval is not register or immediate"; }
    if ( op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safesub(op1.as<x86::Xmm>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safesub(op1.as<x86::Xmm>(), op2.as<x86::Xmm>(), d1, d2);
	else
	if ( op2.isImm() )
	    safesub(op1.as<x86::Xmm>(), op2.as<Imm>(), d1, d2);
	else
	    throw "safesub() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safesub(op1.as<x86::Gp>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safesub(op1.as<x86::Gp>(), op2.as<x86::Xmm>(), d1, d2);
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
    if ( op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	x86::Xmm tmp = cc.newXmm();
	cc.xorpd(tmp, tmp);
	cc.subsd(tmp, op.as<x86::Xmm>());
	cc.movsd(op.as<x86::Xmm>(), tmp);
    }
    else
    if ( op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.neg(op.as<x86::Gp>());
    }
    else
	throw "safeneg() unsupported register type";
}

// perform cc.mul with size casting
void Program::safemul(Operand &op1, Operand &op2, DataDef *d1, DataDef *d2)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec)
    &&   op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( d1 && d1->size == sizeof(float) )
	    cc.mulss(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	    cc.mulsd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	return;
    }
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safemul() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
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
void Program::safediv(Operand &op1, Operand &op2, Operand &op3, DataDef *d1, DataDef *d2, DataDef *d3)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec)
    &&   op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec)
    &&   op3.isReg() && op3.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( d1 && d1->size == sizeof(float) )
	    cc.divss(op2.as<x86::Xmm>(), op3.as<x86::Xmm>());
	else
	    cc.divsd(op2.as<x86::Xmm>(), op3.as<x86::Xmm>());
	return;
    }
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safediv() left operand is not a Gp register";
    if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safediv() middle operand is not a Gp register";
    if ( !op3.isReg() || !op3.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safediv() right operand is not a Gp register";
#if 0
    InvokeNode* call;
    call = cc.call(imm(printint), FuncSignatureT<void, int>(CallConv::kIdHost));
    call->setArg(0, op1.as<x86::Gp>());
    call = cc.call(imm(printint), FuncSignatureT<void, int>(CallConv::kIdHost));
    call->setArg(0, op2.as<x86::Gp>());
    call = cc.call(imm(printint), FuncSignatureT<void, int>(CallConv::kIdHost));
    call->setArg(0, op3.as<x86::Gp>());
#endif
    DBG(cc.comment("safediv() cc.idiv(op1, op2, op3)"));
    // Sign-extend the dividend into the remainder register before idiv.
    // x86's idiv treats rdx:rax as a 128-bit signed dividend; a zeroed
    // rdx plus a negative rax forms a large positive number, producing
    // wildly wrong quotients. Callers used to `xor` the remainder — OK
    // for unsigned, wrong for signed. Use `cqo` to sign-extend rax into
    // rdx for signed types; for unsigned divisions we still want rdx=0,
    // which the caller already arranged via safexor.
    if ( !d2 || !d2->is_unsigned() )
	cc.cqo(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64());
    cc.idiv(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64(), op3.as<x86::Gp>().r64());
}

// perform cc.shl with size casting
void Program::safeshl(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safeshl() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
	throw "safeshl() right operand is not a Gp register or immediate value";
    if ( op2.isImm() )
	cc.shl(op1.as<x86::Gp>(), op2.as<Imm>());
    else
	cc.shl(op1.as<x86::Gp>(), op2.as<x86::Gp>().r8());
}

// perform cc.shr with size casting
void Program::safeshr(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safeshr() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
	throw "safeshr() right operand is not a Gp register or immediate value";
    if ( op2.isImm() )
	cc.shr(op1.as<x86::Gp>(), op2.as<Imm>());
    else
	cc.shr(op1.as<x86::Gp>(), op2.as<x86::Gp>().r8());
}

// perform cc.or_ with size casting
void Program::safeor(Operand &op1, Operand &op2)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    throw "safeor() can only or Xmm with Xmm";
	cc.orpd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	return;
    }
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
	throw "safeor() right operand is not a Gp register or immediate value";
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safeor() left operand is not a Gp register";
    if ( op2.isImm() )
    {
	cc.comment("cc.or_(gp, imm)");
	cc.or_(op1.as<x86::Gp>(), op2.as<Imm>());
    }
    else
    {
	cc.comment("cc.or_(gp, gp)");
	cc.or_(op1.as<x86::Gp>(), op2.as<x86::Gp>());
    }
}

// perform cc.and_ with size casting
void Program::safeand(Operand &op1, Operand &op2)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    throw "safeand() can only and Xmm with Xmm";
	cc.andpd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	return;
    }
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
	throw "safeand() right operand is not a Gp register and immediate value";
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safeand() left operand is not a Gp register";
    if ( op2.isImm() )
    {
	cc.comment("cc.and_(gp, imm)");
	cc.and_(op1.as<x86::Gp>(), op2.as<Imm>());
    }
    else
    {
	cc.comment("cc.and_(gp, gp)");
	cc.and_(op1.as<x86::Gp>(), op2.as<x86::Gp>());
    }
}

// perform cc.xor_ with size casting
void Program::safexor(Operand &op1, Operand &op2)
{
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( !op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    throw "safexor() can only xor Xmm with Xmm";
	cc.xorpd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	return;
    }
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
	throw "safexor() right operand is not a Gp register or immediate value";
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safexor() left operand is not a Gp register";
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
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
#if 0
	x86::Gp tmp = cc.newGpq();
	cc.cvtsd2si(tmp, op.as<x86::Xmm>());
	cc.not_(tmp);
	cc.cvtsi2sd(op.as<x86::Xmm>(), tmp);
#else
	x86::Xmm tmp = cc.newXmm();
	cc.pcmpeqb(tmp, tmp);
	cc.pandn(op.as<x86::Xmm>(), tmp);
#endif
    }
    else
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.not_(op.as<x86::Gp>());

	switch(op.x86RmSize())
	{
	    case 1:  cc.movzx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r8());	break;
	    case 2:  cc.movzx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r16());	break;
//	    case 4:  cc.mov(op.as<x86::Gp>().r32(), op.as<x86::Gp>().r32());	break;
	}
    }
    else
    if ( op.isMem() )
	cc.not_(op.as<x86::Mem>());
    else
	throw "safenot() operand not register";
}

// perform cc.inc with size casting
void Program::safeinc(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	__const_double_1 = cc.newDoubleConst(ConstPoolScope::kLocal, 1.0);
	cc.addsd(op.as<x86::Xmm>(), __const_double_1);
    }
    else
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
	cc.inc(op.as<x86::Gp>());
    else
    if ( op.isMem() )
	cc.inc(op.as<x86::Mem>());
    else
	throw "safeinc() operand not register";
}

// perform cc.dec with size casting
void Program::safedec(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	__const_double_1 = cc.newDoubleConst(ConstPoolScope::kLocal, 1.0);
	cc.subsd(op.as<x86::Xmm>(), __const_double_1);
    }
    else
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
	cc.dec(op.as<x86::Gp>());
    else
    if ( op.isMem() )
	cc.dec(op.as<x86::Mem>());
    else
	throw "safedec() operand not register";
}

void Program::saferet(Operand &op)
{
    if ( !op.isReg() && !op.isImm() ) { throw "saferet() operand is not register or immediate"; }
    if ( op.isReg() )
    {
	if ( op.as<BaseReg>().isGroup(RegGroup::kVec) )
	    cc.ret(op.as<x86::Xmm>());
	else
	if ( op.as<BaseReg>().isGroup(RegGroup::kGp) )
	    cc.ret(op.as<x86::Gp>());
	else
	    throw "saferet() operand is not a supported register type";
    }
    else
    if ( op.isImm() )
    {
	x86::Gp reg = cc.newGpq();
	cc.mov(reg, op.as<Imm>().value());
	cc.ret(reg);
    }
    else
	throw "saferet() unsupported operand";
}


// tests an operand for being equal to zero
void Program::testzero(Operand &op)
{
    if ( op.isMem() )
	cc.cmp(op.as<x86::Mem>(), 0);
    else
    if ( op.isReg() )
    {
	if ( op.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    x86::Xmm tmp = cc.newXmm("testzero_tmp");
	    cc.xorpd(tmp, tmp);
	    cc.ucomisd(op.as<x86::Xmm>(), tmp);
	}
	else
	if ( op.as<BaseReg>().isGroup(RegGroup::kGp) )
	    cc.test(op.as<x86::Gp>(), op.as<x86::Gp>());
	else
	    throw "testzero(op) unsupported register";
    }
    else
	throw "testzero(op) invalid operand";
}

void Program::safeextend(Operand &op, bool unsign)
{
    if ( unsign )
	DBG(cc.comment("safeextend unsigned"));
    else
	DBG(cc.comment("safeextend signed"));
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( unsign )
	{
	    switch(op.x86RmSize())
	    {
		case 1:  cc.movzx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r8());	break;
		case 2:  cc.movzx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r16());	break;
		case 4:  cc.movzx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r32());	break;
	    }
	}
	else
	{
	    switch(op.x86RmSize())
	    {
		case 1:  cc.movsx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r8());	break;
		case 2:  cc.movsx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r16());	break;
		case 4:  cc.movsx(op.as<x86::Gp>().r64(), op.as<x86::Gp>().r32());	break;
	    }
	}
    }
}

// perform a test on two operands
void Program::safetest(Operand &op1, Operand &op2)
{
    DBG(cout << "Program::safetest(" << (uint32_t)op1.opType() << ", " << (uint32_t)op2.opType() << ')' << endl);
    if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    DBG(cc.comment("cc.vtestpd(Xmm, Xmm)"));
	    cc.vtestpd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	}
	else
	if ( op2.isMem() )
	{
	    DBG(cc.comment("cc.vtestpd(Xmm, Mem)"));
	    cc.vtestpd(op1.as<x86::Xmm>(), op2.as<x86::Mem>());
	}
	else
	    throw "safetest(Xmm, op2) is not compatible type (must be Xmm or Mem)";
	return;
    }
    if ( !op1.isReg() )
	throw "safetest(op1, op2) left operand is not a register";
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safetest(op1, op2) left operand is not a supported register";
    if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	DBG(cc.comment("cc.test(Gp, Gp)"));
	cc.test(op1.as<x86::Gp>(), op2.as<x86::Gp>());
    }
    else
    if ( op2.isImm() )
    {
	DBG(cc.comment("cc.test(Gp, Imm)"));
	cc.test(op1.as<x86::Gp>(), op2.as<Imm>());
    }
    else
	throw "safetest(Gp, op2) is not compatible type (must be Gp or Imm)";
}

void Program::safesete(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.sete(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesete() operand not supported";
}

void Program::safesetg(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setg(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetg() operand not supported";
}

void Program::safesetge(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setge(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetge() operand not supported";
}

void Program::safesetl(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setl(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetl() operand not supported";
}

void Program::safesetle(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setle(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetle() operand not supported";
}

void Program::safesetne(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setne(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetne() operand not supported";
}

// Unsigned comparison setcc — setb/setbe/seta/setae. Used when either
// operand of a < / <= / > / >= comparison is unsigned; picking the signed
// variant in that case miscategorises values with the high bit set.
void Program::safesetb(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setb(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetb() operand not supported";
}

void Program::safesetbe(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setbe(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetbe() operand not supported";
}

void Program::safeseta(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.seta(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safeseta() operand not supported";
}

void Program::safesetae(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setae(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetae() operand not supported";
}



// compare two registers even if they are different sizes
void Program::safecmp(x86::Gp &lval, x86::Gp &rval)
{
    if ( lval.x86RmSize() != rval.x86RmSize() )
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
   cc.ucomisd(r1, r2);
//   cc.cmpsd(r1, r2, 0);
}

void Program::safecmp(Operand &op1, Operand &op2)
{
    if ( !op1.isReg() ) { throw "safecmp() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() ) { throw "safecmp() rval is not register or immediate"; }
    if ( op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safecmp(op1.as<x86::Xmm>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safecmp(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	if ( op2.isImm() )
	    throw "safecmp() unable to cmp imm to xmm";
	else
	    throw "safecmp() rval is unsupported";
    }
    else
    if ( op1.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safecmp(op1.as<x86::Gp>(), op2.as<x86::Gp>());
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
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
