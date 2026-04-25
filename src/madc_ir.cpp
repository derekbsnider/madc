// IRBuilder — emit-as-you-build implementation.
//
// Every method on IRBuilder that takes IRValues and emits code calls
// asmjit directly at the point of the call. No deferred graph, no
// second pass. The IR exists to centralize shape/type decisions so
// token compile() methods don't each re-solve them.
//
// Invariant: after any IRBuilder call returns, any result IRValue
// reflects the real state of the asmjit Compiler — its registers are
// allocated, its instructions emitted.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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
#include "madc_ir.h"

using namespace asmjit;

/////////////////////////////////////////////////////////////////////////////
// Private helpers                                                         //
/////////////////////////////////////////////////////////////////////////////

IRValue IRBuilder::newReg(DataDef *type, const char *name_hint)
{
    if ( !type )
	throw "IRBuilder::newReg() type is NULL";
    if ( type->is_real() )
    {
	// Use the type-specific Xmm allocator so asmjit's Compiler
	// register allocator sees a scalar-float / scalar-double type
	// hint instead of the default `int32x4`. Without this, varargs
	// double args interleave through the allocator's int-vector
	// liveness path, which sometimes elides reloads and leaves
	// stale values in xmm0 across calls.
	x86::Xmm xm;
	if ( type->size == sizeof(float) )
	    xm = cc_.newXmmSs("%s", name_hint ? name_hint : "ir_xmm_ss");
	else
	    xm = cc_.newXmmSd("%s", name_hint ? name_hint : "ir_xmm_sd");
	return IRValue::reg(xm, type);
    }
    // Integer or pointer — always allocate a Gpq. Narrower moves still
    // store only the low N bytes, but the vreg lives in a 64-bit slot
    // so downstream widening is free and zero-extended mov to r32
    // gets the full r64 cleared.
    x86::Gp gp = cc_.newGpq("%s", name_hint ? name_hint : "ir_gp");
    return IRValue::reg(gp, type);
}

/////////////////////////////////////////////////////////////////////////////
// load — normalize to Reg shape                                           //
/////////////////////////////////////////////////////////////////////////////

IRValue IRBuilder::load(const IRValue &src)
{
    if ( !src.valid() )
	throw "IRBuilder::load() invalid src";

    // Reg of the same shape: no-op passthrough.
    if ( src.isReg() )
	return src;

    // Mem: size-aware load with sign/zero extension for narrow ints.
    if ( src.isMem() )
    {
	IRValue dst = newReg(src.type, "ir_load");
	const x86::Mem &m = src.op.as<x86::Mem>();
	if ( src.type->is_real() )
	{
	    // Real members come in float (4-byte) or double (8-byte)
	    // flavors. The instruction choice must match the Mem's
	    // actual width, not the DataDef's conceptual width — Mem
	    // size is the x86 ground truth.
	    uint32_t msz = m.size();
	    if ( msz == 0 ) msz = (uint32_t)src.type->size;
	    if ( msz == sizeof(float) )
		cc_.movss(dst.op.as<x86::Xmm>(), m);
	    else
		cc_.movsd(dst.op.as<x86::Xmm>(), m);
	    return dst;
	}
	// Integer or pointer. Widening to Gpq happens via the right
	// extend instruction based on Mem size and source signedness.
	x86::Gp g = dst.op.as<x86::Gp>();
	uint32_t msz = m.size();
	if ( msz == 0 ) msz = (uint32_t)src.type->size;
	bool is_unsigned = src.type->is_unsigned();
	if ( msz == 8 )
	{
	    cc_.mov(g, m);
	}
	else if ( msz == 4 )
	{
	    if ( is_unsigned )
		cc_.mov(g.r32(), m);     // implicit zero-extend 32→64
	    else
		cc_.movsxd(g, m);
	}
	else if ( msz == 2 )
	{
	    if ( is_unsigned )
		cc_.movzx(g, m);
	    else
		cc_.movsx(g, m);
	}
	else if ( msz == 1 )
	{
	    if ( is_unsigned )
		cc_.movzx(g, m);
	    else
		cc_.movsx(g, m);
	}
	else
	{
	    cc_.mov(g, m);               // fallback — 8-byte path
	}
	return dst;
    }

    // Imm: materialize into a fresh Gpq for integer / pointer types.
    // Real-typed immediates (double / float literals) need a
    // constpool round-trip; we defer that until a token port actually
    // needs it so we don't prematurely commit to a pool strategy.
    if ( src.isImm() )
    {
	if ( src.type->is_real() )
	    throw "IRBuilder::load() Imm→Reg for real types not yet implemented";
	IRValue dst = newReg(src.type, "ir_imm");
	cc_.mov(dst.op.as<x86::Gp>(), src.op.as<Imm>());
	return dst;
    }

    // Addr: a Gp holding a pointer. "Loading" an Addr usually means
    // dereferencing — that's a deref() method which Stage 1 will
    // introduce when TokenDeref is ported. Stage 0 just returns the
    // pointer value itself as a Reg of the same type.
    if ( src.isAddr() )
    {
	return IRValue::reg(src.op.as<x86::Gp>(), src.type);
    }

    throw "IRBuilder::load() unhandled shape";
}

/////////////////////////////////////////////////////////////////////////////
// store — write into a Mem or Addr destination                            //
/////////////////////////////////////////////////////////////////////////////

void IRBuilder::store(const IRValue &dst, const IRValue &src)
{
    if ( !dst.valid() || !src.valid() )
	throw "IRBuilder::store() invalid operand";
    if ( !dst.isMem() && !dst.isAddr() )
	throw "IRBuilder::store() dst must be Mem or Addr";

    // Build the effective Mem destination.
    x86::Mem dst_mem;
    if ( dst.isMem() )
    {
	dst_mem = dst.op.as<x86::Mem>();
    }
    else
    {
	// Addr: construct [gp] with the destination type's size.
	dst_mem = x86::ptr(dst.op.as<x86::Gp>(), 0, (uint32_t)dst.type->size);
    }
    if ( dst_mem.size() == 0 && dst.type )
	dst_mem.setSize((uint32_t)dst.type->size);

    // Coerce src to dst.type and load into a Reg if needed. The
    // coerce → load sequence is the common "normalize everything to
    // a Reg of the target type" path; individual fast-paths (e.g.
    // storing an integer Imm directly into a Mem without a temp)
    // can be added later as profiling shows they matter.
    IRValue coerced = coerce(src, dst.type);
    IRValue reg = load(coerced);

    if ( dst.type->is_real() )
    {
	uint32_t dsz = dst_mem.size();
	if ( dsz == sizeof(float) )
	    cc_.movss(dst_mem, reg.op.as<x86::Xmm>());
	else
	    cc_.movsd(dst_mem, reg.op.as<x86::Xmm>());
	return;
    }

    // Integer/pointer store. asmjit picks the instruction size from
    // the Mem width; pass the correct sub-register of the Gpq so the
    // encoding matches.
    x86::Gp g = reg.op.as<x86::Gp>();
    uint32_t dsz = dst_mem.size();
    if      ( dsz == 8 ) cc_.mov(dst_mem, g);
    else if ( dsz == 4 ) cc_.mov(dst_mem, g.r32());
    else if ( dsz == 2 ) cc_.mov(dst_mem, g.r16());
    else if ( dsz == 1 ) cc_.mov(dst_mem, g.r8());
    else                 cc_.mov(dst_mem, g);
}

/////////////////////////////////////////////////////////////////////////////
// coerce — change type while producing a Reg                              //
/////////////////////////////////////////////////////////////////////////////

IRValue IRBuilder::coerce(const IRValue &src, DataDef *to)
{
    if ( !src.valid() )
	throw "IRBuilder::coerce() invalid src";
    if ( !to )
	throw "IRBuilder::coerce() to-type is NULL";

    // Same type: no conversion needed. Just normalize shape to Reg.
    if ( src.type == to )
	return load(src);

    // Fast path: both are integer/pointer of the same size and
    // signedness. asmjit doesn't care which DataDef labels the
    // register; we just relabel.
    bool both_int = src.type->is_integer() && to->is_integer();
    bool both_ptr = src.type->is_pointer() && to->is_pointer();
    if ( (both_int || both_ptr)
      && src.type->size == to->size
      && src.type->is_unsigned() == to->is_unsigned() )
    {
	IRValue r = load(src);
	return IRValue::reg(r.op, to);
    }

    // Integer widening: load src into a Gpq first (load() picks the
    // right sign/zero extend based on src.type), then relabel.
    if ( both_int && to->size >= src.type->size )
    {
	IRValue r = load(src);
	return IRValue::reg(r.op, to);
    }

    // Integer narrowing: the low bytes of the Gpq already hold the
    // truncated value. Downstream store() picks the right sub-reg.
    if ( both_int && to->size < src.type->size )
    {
	IRValue r = load(src);
	return IRValue::reg(r.op, to);
    }

    // Real ↔ real size change (float ↔ double).
    if ( src.type->is_real() && to->is_real() )
    {
	IRValue r = load(src);
	bool src_is_float = (src.type->size == sizeof(float));
	bool dst_is_float = (to->size == sizeof(float));
	if ( src_is_float == dst_is_float )
	    return IRValue::reg(r.op, to);
	IRValue out = newReg(to, "ir_coerce_real");
	if ( src_is_float )
	    cc_.cvtss2sd(out.op.as<x86::Xmm>(), r.op.as<x86::Xmm>());
	else
	    cc_.cvtsd2ss(out.op.as<x86::Xmm>(), r.op.as<x86::Xmm>());
	return out;
    }

    // Integer/pointer → real. We treat pointers like integers here so
    // address-valued arithmetic/casts can still flow through the same
    // normalized path during migration.
    bool src_intish = src.type->is_integer() || src.type->is_pointer();
    bool dst_intish = to->is_integer() || to->is_pointer();
    if ( src_intish && to->is_real() )
    {
	IRValue r = load(src);
	IRValue out = newReg(to, "ir_coerce_i2r");
	if ( to->size == sizeof(float) )
	    cc_.cvtsi2ss(out.op.as<x86::Xmm>(), r.op.as<x86::Gp>());
	else
	    cc_.cvtsi2sd(out.op.as<x86::Xmm>(), r.op.as<x86::Gp>());
	return out;
    }

    // Real → integer/pointer. Current migration only needs the common
    // truncating cast semantics; if later stages need rounded or
    // saturating variants they should become explicit builder ops.
    if ( src.type->is_real() && dst_intish )
    {
	IRValue r = load(src);
	IRValue out = newReg(to, "ir_coerce_r2i");
	if ( src.type->size == sizeof(float) )
	    cc_.cvttss2si(out.op.as<x86::Gp>(), r.op.as<x86::Xmm>());
	else
	    cc_.cvttsd2si(out.op.as<x86::Gp>(), r.op.as<x86::Xmm>());
	return IRValue::reg(out.op, to);
    }

    // Function reference ↔ pointer / function. Both are 8-byte
    // addresses held in a Gp; no conversion beyond a relabel is
    // needed. This covers `DO_FUN *fn = do_who;` and struct-member
    // function-pointer assignments where src is a function label and
    // dst is a typed function-pointer slot.
    bool src_fnptr = src.type->is_function() || src.type->is_pointer();
    bool dst_fnptr = to->is_function() || to->is_pointer();
    if ( src_fnptr && dst_fnptr )
    {
	IRValue r = load(src);
	return IRValue::reg(r.op, to);
    }

    throw "IRBuilder::coerce() unsupported type conversion (not yet implemented)";
}
