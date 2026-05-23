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

// Returns true when the type is a 32-bit integer (int / unsigned int).
// 32-bit x86 ops automatically zero-extend to 64 bits, so using r32
// gives correct wrapping at 2^32 for free.
static inline bool use32(DataDef *type) {
    return type && type->is_integer() && type->size == 4;
}

// Emit a SIMD-width-appropriate move between Xmm and Mem.
// 8-byte vectors use movq; 16-byte use movups.
static inline void emit_simd_mov(x86::Compiler &cc, const Operand &dst, const Operand &src, uint32_t vbytes)
{
    auto id = (vbytes <= 8) ? asmjit::x86::Inst::kIdMovq : asmjit::x86::Inst::kIdMovups;
    cc.emit(id, dst, src);
}

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
		// Unsigned: movzx to r32 implicitly zero-extends to r64.
		// Signed: must use movsx with r64 dest — `movsx r32, r/m8`
		// only sign-extends to 32 bits and zero-extends to 64,
		// which silently corrupts negative values.
		if ( is_unsigned ) cc.movzx(r1.r32(), r2);
		else               cc.movsx(r1, r2);
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
	// Same-or-narrower-dest path. When both operands are the same
	// width, use their natural width so 32-bit ops stay 32-bit.
	// When widths differ, promote both to r64 to satisfy asmjit's
	// validator (it rejects `mov gpw, gpq` with mismatched widths).
	if ( rs == ms )
	    cc.mov(r1, r2);
	else
	    cc.mov(r1.r64(), r2.r64());
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
    if ( d1 && d1->is_simd() )
    {
	if ( d1->size <= 8 ) cc.movq(r1, r2);
	else                 cc.movaps(r1, r2);
	return;
    }
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
    if ( d1 && d1->is_simd() )
    {
	if ( d1->size <= 8 ) cc.movq(r1, r2);
	else                 cc.movups(r1, r2);
	return;
    }
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
		// See safemov(Gp, Gp): movsx r32, r/m16 only sign-extends
		// to 32 bits then zero-extends to 64 — wrong for signed
		// negative values. Use movsx r64, r/m16 for signed.
		if ( is_unsigned ) cc.movzx(r1.r32(), r2);
		else               cc.movsx(r1, r2);
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
		else               cc.movsx(r1, r2);
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
    // Mem destination for a real value: load the double through the
    // constant pool into a scratch Xmm, then store Xmm to Mem. The
    // previous fallback truncated to int via `imm((int)d)`, which
    // silently dropped the fractional part for expressions like
    // `double d = 1.0 + 0.5;` (TokenOperator::optimize constant-folded
    // to 1.5 and then this path stored it as integer 1). Use the
    // destination type hint to pick the Xmm-through-Mem path; callers
    // that genuinely want double→int truncation (rare — explicit int
    // target) still fall through to the imm path below.
    if ( op1.isMem() && d1 && d1->is_real() )
    {
	x86::Mem _const = cc.newDoubleConst(ConstPoolScope::kLocal, d);
	x86::Xmm tmp = cc.newXmmSd("_fconst");
	cc.movsd(tmp, _const);
	DBG(cc.comment("safemov(Mem, double via Xmm)"));
	safemov(op1.as<x86::Mem>(), tmp, d1, d2);
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
    if ( d1 && d1->is_simd() )
    {
	if ( d1->size <= 8 ) cc.movq(m, r2);
	else                 cc.movups(m, r2);
	return;
    }

    // Integer destination: convert Xmm → Gp, then store as integer.
    // Handles `unsigned short s = double_expr;` etc.
    if ( d1 && d1->is_integer() )
    {
	x86::Gp tmp = cc.newGpq("_xmm2int");
	bool src_is_float = d2 && d2->size == sizeof(float);
	if ( src_is_float )
	    cc.cvttss2si(tmp, r2);
	else
	    cc.cvttsd2si(tmp, r2);
	safemov(m, tmp, d1, d1);
	return;
    }

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

    // Absolute-address Mem with a 64-bit displacement (typical for
    // heap-backed file-scope or hoisted-static globals) has no encoding
    // for movss/movsd — the [moffs] form only exists for `mov rax, ...`.
    // Spill the address into a Gp first and use register-base addressing.
    x86::Mem dst = m;
    if ( !dst.hasBase() )
    {
	int64_t off = dst.offset();
	if ( (uint64_t)(off + 0x80000000ll) > 0xFFFFFFFFull )
	{
	    x86::Gp tmp_addr = cc.newIntPtr("_safemov_addr");
	    cc.mov(tmp_addr, asmjit::imm(off));
	    dst = x86::ptr(tmp_addr, 0, ms ? ms : (uint32_t)(d1 ? d1->size : 8));
	}
    }

    if ( dst_is_float )
    {
	if ( !src_is_float )
	{
	    // dst is float, src is double — narrow.
	    x86::Xmm tmp = cc.newXmmSs("_fx_tmp");
	    cc.cvtsd2ss(tmp, r2);
	    cc.movss(dst, tmp);
	}
	else
	    cc.movss(dst, r2);
    }
    else
    {
	if ( src_is_float )
	{
	    // dst is double, src is float — widen.
	    x86::Xmm tmp = cc.newXmmSd("_fx_tmp");
	    cc.cvtss2sd(tmp, r2);
	    cc.movsd(dst, tmp);
	}
	else
	    cc.movsd(dst, r2);
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
		x86::Xmm tmp = (d1 && d1->is_real() && d1->size == sizeof(float)) || (d2 && d2->is_real() && d2->size == sizeof(float))
			   ? cc.newXmmSs("_tmp_mm_ss") : cc.newXmmSd("_tmp_mm_sd");
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

void Program::safeadd(x86::Xmm &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    if ( d1 && d1->is_simd() )
    {
	DataDefSIMD *vdd = static_cast<DataDefSIMD *>(d1);
	if ( vdd->element_type->is_real() )
	{
	    if ( vdd->element_type->size == sizeof(float) ) cc.addps(r1, r2);
	    else                                            cc.addpd(r1, r2);
	}
	else if ( vdd->element_type->size == 1 ) cc.paddb(r1, r2);
	else if ( vdd->element_type->size == 2 ) cc.paddw(r1, r2);
	else if ( vdd->element_type->size == 4 ) cc.paddd(r1, r2);
	else if ( vdd->element_type->size == 8 ) cc.paddq(r1, r2);
	else throw "Program::safeadd() unsupported SIMD element size";
	return;
    }
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

void Program::safeadd(Operand &op1, int i, DataDef *d1, DataDef *d2)
{
    Operand op2 = imm(i);
    safeadd(op1, op2, d1, d2);
}

// should handle all necessary conversions...
void Program::safeadd(Operand &op1, Operand &op2, DataDef *d1, DataDef *d2)
{
    if ( !op1.isReg() ) { cerr << (uint32_t)op1.opType() << endl; throw "safeadd() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() && !op2.isMem() )
	throw "safeadd() rval is not register, immediate, or memory";
    if ( op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safeadd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>(), d1, d2);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    // Gp rhs → convert to Xmm via cvtsi2sd/cvtsi2ss before add.
	    DBG(cc.comment("safeadd() cvtsi2sd op2 Gp -> Xmm"));
	    x86::Xmm tmp = (d1 && d1->size == sizeof(float))
		? cc.newXmmSs("__add_rhs_xmm")
		: cc.newXmmSd("__add_rhs_xmm");
	    if ( d1 && d1->size == sizeof(float) )
		cc.cvtsi2ss(tmp, op2.as<x86::Gp>().r64());
	    else
		cc.cvtsi2sd(tmp, op2.as<x86::Gp>().r64());
	    safeadd(op1.as<x86::Xmm>(), tmp, d1, d1);
	}
	else
	    throw "safeadd() Xmm arithmetic expects an Xmm rhs";
    }
    else
    if ( op1.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    // Mixed Gp + Xmm: promote Gp to Xmm, add, convert back to Gp
	    DBG(cc.comment("safeadd() Gp + Xmm promotion"));
	    x86::Xmm tmp = cc.newXmmSd("__add_lhs_xmm");
	    cc.cvtsi2sd(tmp, op1.as<x86::Gp>().r64());
	    cc.addsd(tmp, op2.as<x86::Xmm>());
	    cc.cvttsd2si(op1.as<x86::Gp>().r64(), tmp);
	}
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safeadd(op1.as<x86::Gp>(), op2.as<x86::Gp>(), d1, d2);
	else
	if ( op2.isImm() )
	    cc.add(op1.as<x86::Gp>(), op2.as<Imm>());
	else
	if ( op2.isMem() )
	{
	    // Stack-resident rhs: load into a Gp first.
	    x86::Gp tmp = cc.newGpq("__add_rhs_gp");
	    safemov(tmp, op2.as<x86::Mem>());
	    safeadd(op1.as<x86::Gp>(), tmp, d1, d2);
	}
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

void Program::safesub(x86::Xmm &r1, x86::Xmm &r2, DataDef *d1, DataDef *d2)
{
    if ( d1 && d1->is_simd() )
    {
	DataDefSIMD *vdd = static_cast<DataDefSIMD *>(d1);
	if ( vdd->element_type->is_real() )
	{
	    if ( vdd->element_type->size == sizeof(float) ) cc.subps(r1, r2);
	    else                                            cc.subpd(r1, r2);
	}
	else if ( vdd->element_type->size == 1 ) cc.psubb(r1, r2);
	else if ( vdd->element_type->size == 2 ) cc.psubw(r1, r2);
	else if ( vdd->element_type->size == 4 ) cc.psubd(r1, r2);
	else if ( vdd->element_type->size == 8 ) cc.psubq(r1, r2);
	else throw "Program::safesub() unsupported SIMD element size";
	return;
    }
    if ( d1 && d1->size == sizeof(float) )
	cc.subss(r1, r2);
    else
	cc.subsd(r1, r2);
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
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safesub(op1.as<x86::Xmm>(), op2.as<x86::Xmm>(), d1, d2);
	else if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    x86::Xmm tmp = cc.newXmmSd("__sub_rhs_xmm");
	    cc.cvtsi2sd(tmp, op2.as<x86::Gp>().r64());
	    safesub(op1.as<x86::Xmm>(), tmp, d1, d1);
	}
	else
	    throw "safesub() Xmm arithmetic expects an Xmm rhs";
    }
    else
    if ( op1.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    x86::Xmm tmp = cc.newXmmSd("__sub_lhs_xmm");
	    cc.cvtsi2sd(tmp, op1.as<x86::Gp>().r64());
	    cc.subsd(tmp, op2.as<x86::Xmm>());
	    cc.cvttsd2si(op1.as<x86::Gp>().r64(), tmp);
	}
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safesub(op1.as<x86::Gp>(), op2.as<x86::Gp>(), d1, d2);
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

void Program::safeneg(Operand &op, DataDef *dd)
{
    if ( !op.isReg() ) { throw "safeneg() lval is not a register"; }
    if ( op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( dd && dd->is_simd() )
	{
	    DataDefSIMD *vdd = static_cast<DataDefSIMD *>(dd);
	    x86::Xmm zero = cc.newXmm("safeneg_zero");
	    cc.pxor(zero, zero);
	    if ( vdd->element_type->is_real() )
	    {
		// Packed float/double negation: subtract from zero
		if ( vdd->element_type->size == sizeof(float) )
		    cc.subps(zero, op.as<x86::Xmm>());
		else
		    cc.subpd(zero, op.as<x86::Xmm>());
	    }
	    else if ( vdd->element_type->size == 1 )
		cc.psubb(zero, op.as<x86::Xmm>());
	    else if ( vdd->element_type->size == 2 )
		cc.psubw(zero, op.as<x86::Xmm>());
	    else if ( vdd->element_type->size == 4 )
		cc.psubd(zero, op.as<x86::Xmm>());
	    else
		cc.psubq(zero, op.as<x86::Xmm>());
	    emit_simd_mov(cc, op.as<x86::Xmm>(), zero, (uint32_t)vdd->size);
	}
	else
	{
	    bool is_float = dd && dd->size == sizeof(float);
	    if ( is_float )
	    {
		x86::Gp mask_gp = cc.newGpd("safeneg_mask");
		cc.mov(mask_gp, imm(0x80000000u));
		x86::Xmm mask = cc.newXmmSs("safeneg_mask");
		cc.movd(mask, mask_gp);
		cc.xorps(op.as<x86::Xmm>(), mask);
	    }
	    else
	    {
		x86::Gp mask_gp = cc.newGpq("safeneg_mask");
		cc.mov(mask_gp, imm((int64_t)0x8000000000000000ULL));
		x86::Xmm mask = cc.newXmmSd("safeneg_mask");
		cc.movq(mask, mask_gp);
		cc.xorpd(op.as<x86::Xmm>(), mask);
	    }
	}
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
    // Mixed int/float promotion: convert the integer operand to Xmm
    // and perform floating-point multiplication
    bool lhs_is_gp = op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kGp);
    bool rhs_is_gp = op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp);
    bool lhs_is_xmm = op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec);
    bool rhs_is_xmm = op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec);

    if ( lhs_is_gp && rhs_is_xmm )
    {
	// Usual arithmetic conversion: int * double → promote int to double,
	// multiply as doubles, convert back to int (truncate)
	x86::Xmm tmp = cc.newXmm("_mul_promote");
	cc.cvtsi2sd(tmp, op1.as<x86::Gp>());
	cc.mulsd(tmp, op2.as<x86::Xmm>());
	cc.cvttsd2si(op1.as<x86::Gp>().r64(), tmp);
	return;
    }
    if ( lhs_is_xmm && rhs_is_gp )
    {
	x86::Xmm tmp = cc.newXmm("_mul_promote");
	cc.cvtsi2sd(tmp, op2.as<x86::Gp>());
	cc.mulsd(op1.as<x86::Xmm>(), tmp);
	return;
    }
    if ( lhs_is_xmm && rhs_is_xmm )
    {
	if ( d1 && d1->is_simd() )
	{
	    DataDefSIMD *vdd = static_cast<DataDefSIMD *>(d1);
	    if ( vdd->element_type->is_real() )
	    {
		if ( vdd->element_type->size == sizeof(float) )
		    cc.mulps(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
		else
		    cc.mulpd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	    }
	    else if ( vdd->element_type->size == 1 )
	    {
		// No SSE pmullb; unpack to words, multiply, repack
		x86::Xmm zero = cc.newXmm("_mul_zero");
		x86::Xmm lo1 = cc.newXmm("_mul_lo1");
		x86::Xmm lo2 = cc.newXmm("_mul_lo2");
		cc.pxor(zero, zero);
		cc.emit(asmjit::x86::Inst::kIdMovaps, lo1, op1.as<x86::Xmm>());
		cc.emit(asmjit::x86::Inst::kIdMovaps, lo2, op2.as<x86::Xmm>());
		// Unpack low bytes to words
		cc.punpcklbw(lo1, zero);
		cc.punpcklbw(lo2, zero);
		cc.pmullw(lo1, lo2);
		// Unpack high bytes to words
		x86::Xmm hi1 = cc.newXmm("_mul_hi1");
		x86::Xmm hi2 = cc.newXmm("_mul_hi2");
		cc.emit(asmjit::x86::Inst::kIdMovaps, hi1, op1.as<x86::Xmm>());
		cc.emit(asmjit::x86::Inst::kIdMovaps, hi2, op2.as<x86::Xmm>());
		cc.punpckhbw(hi1, zero);
		cc.punpckhbw(hi2, zero);
		cc.pmullw(hi1, hi2);
		// Pack results back to bytes (unsigned saturation then mask)
		cc.packuswb(lo1, hi1);
		cc.emit(asmjit::x86::Inst::kIdMovaps, op1.as<x86::Xmm>(), lo1);
	    }
	    else if ( vdd->element_type->size == 2 )
		cc.pmullw(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	    else if ( vdd->element_type->size == 4 )
		cc.pmulld(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	    else if ( vdd->element_type->size == 8 )
	    {
		// No SSE pmullq; extract lanes, imul, reinsert
		uint32_t vbytes = (uint32_t)vdd->size;
		x86::Mem lhs_slot = cc.newStack(vbytes, (uint32_t)vdd->alignment());
		x86::Mem rhs_slot = cc.newStack(vbytes, (uint32_t)vdd->alignment());
		lhs_slot.setSize(vbytes);
		rhs_slot.setSize(vbytes);
		emit_simd_mov(cc, lhs_slot, op1.as<x86::Xmm>(), vbytes);
		emit_simd_mov(cc, rhs_slot, op2.as<x86::Xmm>(), vbytes);
		x86::Gp a = cc.newGpq("_mul64_a");
		x86::Gp b = cc.newGpq("_mul64_b");
		for ( size_t i = 0; i < vdd->lane_count; ++i )
		{
		    x86::Mem la = lhs_slot; la.addOffset((int64_t)(i*8)); la.setSize(8);
		    x86::Mem lb = rhs_slot; lb.addOffset((int64_t)(i*8)); lb.setSize(8);
		    cc.mov(a, la);
		    cc.mov(b, lb);
		    cc.imul(a, b);
		    cc.mov(la, a);
		}
		lhs_slot.setSize(vbytes);
		emit_simd_mov(cc, op1.as<x86::Xmm>(), lhs_slot, vbytes);
	    }
	    else
		throw "safemul() unsupported SIMD integer element size";
	    return;
	}
	if ( d1 && d1->size == sizeof(float) )
	    cc.mulss(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	else
	    cc.mulsd(op1.as<x86::Xmm>(), op2.as<x86::Xmm>());
	return;
    }
    if ( !op1.isReg() || !lhs_is_gp )
	throw "safemul() left operand is not a Gp register";
    if ( !op2.isImm() && !rhs_is_gp )
	throw "safemul() right operand is not a Gp register or immediate value";
    // Use 32-bit imul for 32-bit types so results wrap at 2^32.
    bool w32 = use32(d1);
    if ( op2.isImm() )
    {
	if ( w32 ) cc.imul(op1.as<x86::Gp>().r32(), op2.as<Imm>());
	else       cc.imul(op1.as<x86::Gp>().r64(), op2.as<Imm>());
    }
    else
    {
	if ( w32 ) cc.imul(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r32());
	else       cc.imul(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64());
    }
}

#if 0
void printint(int i)
{
    cout << "printint: " << i << endl;
}
#endif

// perform cc.div with size casting
//
// op2 (the dividend) is the destination register that receives the quotient
// for both x86 idiv (writes quotient into rax) and SSE divsd/divss (writes
// op2 := op2 / op3). Its register family therefore selects the result
// family: Xmm op2 → real division, Gp op2 → integer division. Mixed-family
// op3 (and op1 in the Xmm path) is coerced into the chosen family before the
// hardware op so SMAUG idioms that compute one side as Xmm and the other as
// Gp don't fall through to a raw throw.
void Program::safediv(Operand &op1, Operand &op2, Operand &op3, DataDef *d1, DataDef *d2, DataDef *d3)
{
    bool op2_is_xmm = op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec);
    bool op2_is_gp  = op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp);

    if ( op2_is_xmm )
    {
	// Real division. Coerce op3 into Xmm if it's Gp.
	if ( !op3.isReg() )
	    throw "safediv() right operand is not a register";
	x86::Xmm divisor_xmm;
	if ( op3.as<BaseReg>().isGroup(RegGroup::kVec) )
	    divisor_xmm = op3.as<x86::Xmm>();
	else if ( op3.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    DBG(cc.comment("safediv() cvtsi2sd op3 Gp -> Xmm"));
	    divisor_xmm = (d1 && d1->size == sizeof(float))
		? cc.newXmmSs("__div_rhs_xmm")
		: cc.newXmmSd("__div_rhs_xmm");
	    if ( d1 && d1->size == sizeof(float) )
		cc.cvtsi2ss(divisor_xmm, op3.as<x86::Gp>().r64());
	    else
		cc.cvtsi2sd(divisor_xmm, op3.as<x86::Gp>().r64());
	}
	else
	    throw "safediv() right operand is not a Gp or Xmm register";

	if ( d1 && d1->is_simd() )
	{
	    DataDefSIMD *vdd = static_cast<DataDefSIMD *>(d1);
	    if ( vdd->element_type->is_real() )
	    {
		if ( vdd->element_type->size == sizeof(float) )
		    cc.divps(op2.as<x86::Xmm>(), divisor_xmm);
		else
		    cc.divpd(op2.as<x86::Xmm>(), divisor_xmm);
	    }
	    else
	    {
		// No SSE integer division instruction — extract each lane,
		// divide as scalar, reinsert.
		size_t lane_count = vdd->lane_count;
		size_t elem_size = vdd->element_type->size;
		x86::Gp lhs_gp = cc.newGpq("_div_lane_lhs");
		x86::Gp rhs_gp = cc.newGpq("_div_lane_rhs");
		x86::Gp rem_gp = cc.newGpq("_div_lane_rem");
		uint32_t vbytes = (uint32_t)vdd->size;
		x86::Mem lhs_slot = cc.newStack(vbytes, (uint32_t)vdd->alignment());
		x86::Mem rhs_slot = cc.newStack(vbytes, (uint32_t)vdd->alignment());
		x86::Mem rem_slot = cc.newStack(vbytes, (uint32_t)vdd->alignment());
		lhs_slot.setSize(vbytes);
		rhs_slot.setSize(vbytes);
		rem_slot.setSize(vbytes);
		emit_simd_mov(cc, lhs_slot, op2.as<x86::Xmm>(), vbytes);
		emit_simd_mov(cc, rhs_slot, divisor_xmm, vbytes);
		for ( size_t i = 0; i < lane_count; ++i )
		{
		    x86::Mem ll = lhs_slot;
		    ll.addOffset((int64_t)(i * elem_size));
		    ll.setSize((uint32_t)elem_size);
		    x86::Mem rl = rhs_slot;
		    rl.addOffset((int64_t)(i * elem_size));
		    rl.setSize((uint32_t)elem_size);
		    bool is_unsigned = vdd->element_type->is_unsigned();
		    if ( elem_size <= 2 )
		    {
			if ( is_unsigned )
			{
			    cc.movzx(lhs_gp.r32(), ll);
			    cc.movzx(rhs_gp.r32(), rl);
			}
			else
			{
			    cc.movsx(lhs_gp.r32(), ll);
			    cc.movsx(rhs_gp.r32(), rl);
			}
		    }
		    else if ( elem_size == 4 )
		    {
			cc.mov(lhs_gp.r32(), ll);
			cc.mov(rhs_gp.r32(), rl);
		    }
		    else
		    {
			cc.mov(lhs_gp.r64(), ll);
			cc.mov(rhs_gp.r64(), rl);
		    }
		    cc.xor_(rem_gp, rem_gp);
		    if ( is_unsigned )
		    {
			if ( elem_size <= 4 )
			    cc.div(rem_gp.r32(), lhs_gp.r32(), rhs_gp.r32());
			else
			    cc.div(rem_gp.r64(), lhs_gp.r64(), rhs_gp.r64());
		    }
		    else
		    {
			// Sign-extend for signed division
			if ( elem_size <= 4 )
			{
			    cc.cdq(rem_gp.r32(), lhs_gp.r32());
			    cc.idiv(rem_gp.r32(), lhs_gp.r32(), rhs_gp.r32());
			}
			else
			{
			    cc.cqo(rem_gp.r64(), lhs_gp.r64());
			    cc.idiv(rem_gp.r64(), lhs_gp.r64(), rhs_gp.r64());
			}
		    }
		    // Store quotient and remainder back
		    x86::Mem rl2 = rem_slot;
		    rl2.addOffset((int64_t)(i * elem_size));
		    rl2.setSize((uint32_t)elem_size);
		    if ( elem_size == 1 )
		    {
			cc.mov(ll, lhs_gp.r8());
			cc.mov(rl2, rem_gp.r8());
		    }
		    else if ( elem_size == 2 )
		    {
			cc.mov(ll, lhs_gp.r16());
			cc.mov(rl2, rem_gp.r16());
		    }
		    else if ( elem_size == 4 )
		    {
			cc.mov(ll, lhs_gp.r32());
			cc.mov(rl2, rem_gp.r32());
		    }
		    else
		    {
			cc.mov(ll, lhs_gp.r64());
			cc.mov(rl2, rem_gp.r64());
		    }
		}
		lhs_slot.setSize(vbytes);
		rem_slot.setSize(vbytes);
		emit_simd_mov(cc, op2.as<x86::Xmm>(), lhs_slot, vbytes);
		if ( op1.isReg() && op1.as<BaseReg>().isGroup(RegGroup::kVec) )
		    emit_simd_mov(cc, op1.as<x86::Xmm>(), rem_slot, vbytes);
	    }
	    return;
	}
	if ( d1 && d1->size == sizeof(float) )
	    cc.divss(op2.as<x86::Xmm>(), divisor_xmm);
	else
	    cc.divsd(op2.as<x86::Xmm>(), divisor_xmm);
	return;
    }

    // Integer division: op1 (remainder/rdx) and op2 (dividend/rax) must be Gp.
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safediv() left operand is not a Gp register";
    if ( !op2_is_gp )
	throw "safediv() middle operand is not a Gp register";

    x86::Gp divisor_gp;
    if ( op3.isReg() && op3.as<BaseReg>().isGroup(RegGroup::kGp) )
	divisor_gp = op3.as<x86::Gp>();
    else if ( op3.isReg() && op3.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	DBG(cc.comment("safediv() cvttsd2si op3 Xmm -> Gp"));
	divisor_gp = cc.newGpq("__div_rhs_gp");
	if ( d3 && d3->size == sizeof(float) )
	    cc.cvttss2si(divisor_gp, op3.as<x86::Xmm>());
	else
	    cc.cvttsd2si(divisor_gp, op3.as<x86::Xmm>());
    }
    else
	throw "safediv() right operand is not a Gp or Xmm register";

    DBG(cc.comment("safediv() cc.idiv(op1, op2, op3)"));
    // Sign-extend the dividend into the remainder register before idiv.
    // x86's idiv treats rdx:rax as a 128-bit signed dividend; a zeroed
    // rdx plus a negative rax forms a large positive number, producing
    // wildly wrong quotients. Callers used to `xor` the remainder — OK
    // for unsigned, wrong for signed. Use `cqo` to sign-extend rax into
    // rdx for signed types; for unsigned divisions we still want rdx=0,
    // which the caller already arranged via safexor.
    // Pick 32-bit or 64-bit division to match the operand width.
    // 32-bit: cdq (sign-extend eax→edx:eax) + idiv r32, or div r32.
    // 64-bit: cqo (sign-extend rax→rdx:rax) + idiv r64, or div r64.
    bool use32 = d2 && d2->is_integer() && d2->size == 4;
    if ( d2 && d2->is_unsigned() )
    {
	if ( use32 )
	    cc.div(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r32(), divisor_gp.r32());
	else
	    cc.div(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64(), divisor_gp.r64());
    }
    else
    {
	if ( use32 )
	{
	    cc.cdq(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r32());
	    cc.idiv(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r32(), divisor_gp.r32());
	}
	else
	{
	    cc.cqo(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64());
	    cc.idiv(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64(), divisor_gp.r64());
	}
    }
}

// perform cc.shl with size casting
void Program::safeshl(Operand &op1, Operand &op2, DataDef *type)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safeshl() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
	throw "safeshl() right operand is not a Gp register or immediate value";
    // Use 32-bit shift for sub-64-bit types so the result wraps at the
    // correct width (x86: 32-bit ops implicitly zero-extend to 64).
    bool use32 = type && type->is_integer() && type->size == 4;
    if ( op2.isImm() )
    {
	if ( use32 )
	    cc.shl(op1.as<x86::Gp>().r32(), op2.as<Imm>());
	else
	    cc.shl(op1.as<x86::Gp>().r64(), op2.as<Imm>());
    }
    else
    {
	if ( use32 )
	    cc.shl(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r8());
	else
	    cc.shl(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r8());
    }
}

// perform cc.shr with size casting
void Program::safeshr(Operand &op1, Operand &op2, DataDef *type)
{
    if ( !op1.isReg() || !op1.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "safeshr() left operand is not a Gp register";
    if ( !op2.isImm() && (!op2.isReg() || !op2.as<BaseReg>().isGroup(RegGroup::kGp)) )
	throw "safeshr() right operand is not a Gp register or immediate value";
    // Use SAR (arithmetic shift) for signed types to preserve the sign
    // bit. Use SHR (logical shift) for unsigned types.
    // Use 32-bit shift for 32-bit types so results wrap correctly.
    bool use_sar = type && !type->is_unsigned();
    bool w32 = use32(type);
    if ( op2.isImm() )
    {
	if ( use_sar )
	{
	    if ( w32 ) cc.sar(op1.as<x86::Gp>().r32(), op2.as<Imm>());
	    else       cc.sar(op1.as<x86::Gp>().r64(), op2.as<Imm>());
	}
	else
	{
	    if ( w32 ) cc.shr(op1.as<x86::Gp>().r32(), op2.as<Imm>());
	    else       cc.shr(op1.as<x86::Gp>().r64(), op2.as<Imm>());
	}
    }
    else
    {
	if ( use_sar )
	{
	    if ( w32 ) cc.sar(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r8());
	    else       cc.sar(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r8());
	}
	else
	{
	    if ( w32 ) cc.shr(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r8());
	    else       cc.shr(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r8());
	}
    }
}

// perform cc.or_ with size casting
void Program::safeor(Operand &op1, Operand &op2, DataDef *type)
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
    bool w32 = use32(type);
    if ( op2.isImm() )
    {
	if ( w32 ) cc.or_(op1.as<x86::Gp>().r32(), op2.as<Imm>());
	else       cc.or_(op1.as<x86::Gp>().r64(), op2.as<Imm>());
    }
    else
    {
	if ( w32 ) cc.or_(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r32());
	else       cc.or_(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64());
    }
}

// perform cc.and_ with size casting
void Program::safeand(Operand &op1, Operand &op2, DataDef *type)
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
    bool w32 = use32(type);
    if ( op2.isImm() )
    {
	if ( w32 ) cc.and_(op1.as<x86::Gp>().r32(), op2.as<Imm>());
	else       cc.and_(op1.as<x86::Gp>().r64(), op2.as<Imm>());
    }
    else
    {
	if ( w32 ) cc.and_(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r32());
	else       cc.and_(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64());
    }
}

// perform cc.xor_ with size casting
void Program::safexor(Operand &op1, Operand &op2, DataDef *type)
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
    bool w32 = use32(type);
    if ( op2.isImm() )
    {
	if ( w32 ) cc.xor_(op1.as<x86::Gp>().r32(), op2.as<Imm>());
	else       cc.xor_(op1.as<x86::Gp>().r64(), op2.as<Imm>());
    }
    else
    {
	if ( w32 ) cc.xor_(op1.as<x86::Gp>().r32(), op2.as<x86::Gp>().r32());
	else       cc.xor_(op1.as<x86::Gp>().r64(), op2.as<x86::Gp>().r64());
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
    if ( !op.isReg() && !op.isImm() && !op.isMem() )
	throw "saferet() operand is not register, immediate, or memory";
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
    if ( op.isMem() )
    {
	// Stack-resident value: load into a Gp first, then ret.
	// SMAUG `fread_bitvector` returns a struct-typed local where the
	// IR pipeline left the value as a Mem operand.
	x86::Gp reg = cc.newGpq("__ret_mem");
	cc.mov(reg, op.as<x86::Mem>());
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
	    x86::Xmm tmp = cc.newXmmSd("testzero_tmp");
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

void Program::safesetp(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setp(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetp() operand not supported";
}

void Program::safesetnp(Operand &op)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	cc.setnp(op.as<x86::Gp>().r8());
	if ( op.x86RmSize() > 1 )
	    cc.movzx(op.as<x86::Gp>(), op.as<x86::Gp>().r8());
    }
    else
	throw "safesetnp() operand not supported";
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
    uint32_t ls = lval.x86RmSize();
    uint32_t rs = rval.x86RmSize();
    if ( ls == rs )
    {
	cc.cmp(lval, rval);
	return;
    }
    // Sub-Gpq operand: the parent Gpq's high bytes are stale (e.g. from
    // a `neg al` after `mov rax, 1` — the low byte is correct but bits
    // 8..63 still hold the unrelated `1`).  Comparing as r64()'s of the
    // parent vregs would read those stale bits.  Sign-extend the
    // narrower operand into a fresh Gpq so the compare sees its true
    // value.  Default to signed extension — that's the C-standard
    // semantic for `int x = c;` style widening, and unsigned values
    // happen to agree on the low byte either way at the cmp site.
    auto extend = [&](x86::Gp &g) -> x86::Gp {
	x86::Gp t = cc.newGpq("_cmpext");
	switch ( g.x86RmSize() )
	{
	    case 1: cc.movsx(t, g.r8());  break;
	    case 2: cc.movsx(t, g.r16()); break;
	    case 4: cc.movsxd(t, g.r32()); break;
	    default: cc.mov(t, g.r64());  break;
	}
	return t;
    };
    if ( ls < 8 && rs < 8 )
    {
	x86::Gp lt = extend(lval);
	x86::Gp rt = extend(rval);
	cc.cmp(lt, rt);
    }
    else if ( ls < 8 )
    {
	x86::Gp lt = extend(lval);
	cc.cmp(lt, rval.r64());
    }
    else
    {
	x86::Gp rt = extend(rval);
	cc.cmp(lval.r64(), rt);
    }
}

void Program::safecmp(x86::Xmm &r1, x86::Xmm &r2, DataDef *dd)
{
    if ( dd && dd->size == sizeof(float) )
	cc.ucomiss(r1, r2);
    else
	cc.ucomisd(r1, r2);
}

void Program::safecmp(Operand &op1, Operand &op2, DataDef *dd)
{
    if ( !op1.isReg() ) { throw "safecmp() lval is not a register"; }
    if ( !op2.isReg() && !op2.isImm() && !op2.isMem() )
	{ throw "safecmp() rval is not register, immediate, or memory"; }
    if ( op1.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	    safecmp(op1.as<x86::Xmm>(), op2.as<x86::Xmm>(), dd);
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    if ( dd && dd->size == sizeof(float) )
	    {
		x86::Xmm tmp = cc.newXmmSs("__cmp_rhs_as_flt");
		cc.cvtsi2ss(tmp, op2.as<x86::Gp>());
		cc.ucomiss(op1.as<x86::Xmm>(), tmp);
	    }
	    else
	    {
		x86::Xmm tmp = cc.newXmmSd("__cmp_rhs_as_dbl");
		cc.cvtsi2sd(tmp, op2.as<x86::Gp>());
		cc.ucomisd(op1.as<x86::Xmm>(), tmp);
	    }
	}
	else
	if ( op2.isMem() )
	{
	    x86::Xmm tmp = (dd && dd->size == sizeof(float))
		? cc.newXmmSs("__cmp_rhs_flt")
		: cc.newXmmSd("__cmp_rhs_dbl");
	    safemov(tmp, op2.as<x86::Mem>());
	    safecmp(op1.as<x86::Xmm>(), tmp, dd);
	}
	else
	    throw "safecmp() Xmm compare expects an Xmm rhs";
    }
    else
    if ( op1.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kGp) )
	    safecmp(op1.as<x86::Gp>(), op2.as<x86::Gp>());
	else
	if ( op2.isImm() )
	    cc.cmp(op1.as<x86::Gp>(), op2.as<Imm>());
	else
	if ( op2.isMem() )
	{
	    // Gp vs Mem comparison: load Mem into a Gp first to ensure
	    // matching widths (sub-qword loads need movzx/movsx). This
	    // handles the common SMAUG idiom of comparing a computed
	    // value against a stack-resident local or a struct member.
	    x86::Gp tmp = cc.newGpq("__cmp_rhs");
	    safemov(tmp, op2.as<x86::Mem>());
	    safecmp(op1.as<x86::Gp>(), tmp);
	}
	else
	if ( op2.isReg() && op2.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    // Gp vs Xmm: convert the integer to the matching real type.
	    if ( dd && dd->size == sizeof(float) )
	    {
		x86::Xmm tmp = cc.newXmmSs("__cmp_int_as_flt");
		cc.cvtsi2ss(tmp, op1.as<x86::Gp>());
		cc.ucomiss(tmp, op2.as<x86::Xmm>());
	    }
	    else
	    {
		x86::Xmm tmp = cc.newXmmSd("__cmp_int_as_dbl");
		cc.cvtsi2sd(tmp, op1.as<x86::Gp>());
		cc.ucomisd(tmp, op2.as<x86::Xmm>());
	    }
	}
	else
	    throw "safecmp() rval is unsupported";
    }
    else
    {
	throw "safecmp() lval unsupported type";
    }
}
