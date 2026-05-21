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
    // 32-bit integer types use Gpd so arithmetic wraps at 2^32 naturally.
    // Sub-32-bit types (char, short) and 64-bit types use Gpq — 8/16-bit
    // x86 ops don't clear upper bits, so those need explicit extension.
    // Pointers always use Gpq (64-bit addresses).
    if ( type && type->is_integer() && type->size == 4 )
    {
	x86::Gp gp = cc_.newGpd("%s", name_hint ? name_hint : "ir_gp32");
	return IRValue::reg(gp, type);
    }
    x86::Gp gp = cc_.newGpq("%s", name_hint ? name_hint : "ir_gp");
    return IRValue::reg(gp, type);
}

static x86::Gp materialize_string_object_ptr(x86::Compiler &cc, const IRValue &src)
{
    if ( src.isMem() )
    {
	x86::Gp gp = cc.newIntPtr("ir_str_obj");
	cc.lea(gp, src.op.as<x86::Mem>());
	return gp;
    }
    if ( src.isReg() )
	return src.op.as<x86::Gp>();
    if ( src.isAddr() )
	return src.op.as<x86::Gp>();
    throw "materialize_string_object_ptr() unsupported string operand shape";
}

static IRValue canonicalize_narrow_integer_reg(x86::Compiler &cc, const IRValue &src,
					       DataDef *type)
{
    if ( !type || !type->is_integer() || type->size >= 8 )
	return IRValue::reg(src.op, type ? type : src.type);
    if ( !src.isReg() || !src.op.as<BaseReg>().isGroup(RegGroup::kGp) )
	return IRValue::reg(src.op, type);

    x86::Gp in = src.op.as<x86::Gp>();
    // Use Gpd for 4-byte types, Gpq for everything else.
    x86::Gp out = (type->size == 4)
	? cc.newGpd("ir_narrow32") : cc.newGpq("ir_narrow");
    bool is_unsigned = type->is_unsigned();
    switch ( type->size )
    {
	case 1:
	    if ( is_unsigned ) cc.movzx(out, in.r8());
	    else               cc.movsx(out, in.r8());
	    break;
	case 2:
	    if ( is_unsigned ) cc.movzx(out, in.r16());
	    else               cc.movsx(out, in.r16());
	    break;
	case 3:
	    cc.mov(out.r32(), in.r32());
	    if ( is_unsigned )
		cc.and_(out.r32(), imm(0x00ffffff));
	    else
	    {
		cc.shl(out.r32(), imm(8));
		cc.sar(out.r32(), imm(8));
		if ( out.isGpq() ) cc.movsxd(out, out.r32());
	    }
	    break;
	case 4:
	    // Gpd output: mov r32,r32 truncates and zero-extends.
	    cc.mov(out.r32(), in.r32());
	    break;
	default:
	    cc.mov(out, in);
	    break;
    }
    return IRValue::reg(out, type);
}

static IRValue coerce_unsigned_int_to_real(x86::Compiler &cc, const IRValue &src, DataDef *to)
{
    if ( !src.valid() || !src.isReg() || !src.op.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "coerce_unsigned_int_to_real() expects Gp register source";
    if ( !to || !to->is_real() )
	throw "coerce_unsigned_int_to_real() expects real destination";

    x86::Gp gp = src.op.as<x86::Gp>();
    x86::Xmm out_xmm = (to->size == sizeof(float))
	? cc.newXmmSs("ir_coerce_u2r_ss")
	: cc.newXmmSd("ir_coerce_u2r_sd");
    IRValue out = IRValue::reg(out_xmm, to);

    if ( src.type && src.type->size == 8 )
    {
	Label lbl_positive = cc.newLabel();
	Label lbl_done = cc.newLabel();
	cc.test(gp.r64(), gp.r64());
	cc.jns(lbl_positive);

	x86::Gp tmp = cc.newGpq("ir_u64_halved");
	cc.mov(tmp, gp.r64());
	x86::Gp low_bit = cc.newGpq("ir_u64_low");
	cc.mov(low_bit, gp.r64());
	cc.and_(low_bit, imm(1));
	cc.shr(tmp, imm(1));
	cc.or_(tmp, low_bit);
	if ( to->size == sizeof(float) )
	{
	    cc.cvtsi2ss(out.op.as<x86::Xmm>(), tmp.r64());
	    cc.addss(out.op.as<x86::Xmm>(), out.op.as<x86::Xmm>());
	}
	else
	{
	    cc.cvtsi2sd(out.op.as<x86::Xmm>(), tmp.r64());
	    cc.addsd(out.op.as<x86::Xmm>(), out.op.as<x86::Xmm>());
	}
	cc.jmp(lbl_done);
	cc.bind(lbl_positive);
	if ( to->size == sizeof(float) )
	    cc.cvtsi2ss(out.op.as<x86::Xmm>(), gp.r64());
	else
	    cc.cvtsi2sd(out.op.as<x86::Xmm>(), gp.r64());
	cc.bind(lbl_done);
	return out;
    }

    // Zero-extend 32-bit unsigned sources into a 64-bit register before
    // conversion. cvtsi2s{sd,ss} on a Gpd interprets the input as signed.
    if ( src.type && src.type->size == 4 )
    {
	x86::Gp wide = cc.newGpq("ir_u32_wide");
	cc.mov(wide.r32(), gp.r32());
	gp = wide;
	if ( to->size == sizeof(float) )
	    cc.cvtsi2ss(out.op.as<x86::Xmm>(), gp.r64());
	else
	    cc.cvtsi2sd(out.op.as<x86::Xmm>(), gp.r64());
	return out;
    }

    if ( to->size == sizeof(float) )
	cc.cvtsi2ss(out.op.as<x86::Xmm>(), gp.r64());
    else
	cc.cvtsi2sd(out.op.as<x86::Xmm>(), gp.r64());
    return out;
}

/////////////////////////////////////////////////////////////////////////////
// load — normalize to Reg shape                                           //
/////////////////////////////////////////////////////////////////////////////

IRValue IRBuilder::load(const IRValue &src)
{
    if ( !src.valid() )
	throw "IRBuilder::load() invalid src";

    // Reg values: integer virtual registers are Gpq (64-bit) or Gpd
    // (32-bit for int/uint). Sub-width types need sign/zero extension.
    if ( src.isReg() )
    {
	if ( src.type && src.type->is_integer() && src.type->size < 8
	  && src.op.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    x86::Gp g = src.op.as<x86::Gp>();
	    bool is_unsigned = src.type->is_unsigned();
	    if ( src.type->size == 4 )
	    {
		// Gpd vregs are already canonical — skip extension.
		if ( g.isGpd() )
		    { /* already canonical 32-bit */ }
		else if ( is_unsigned )
		    cc_.mov(g.r32(), g.r32());
		else
		    cc_.movsxd(g, g.r32());
	    }
	    else if ( src.type->size == 2 )
	    {
		if ( is_unsigned )
		    cc_.movzx(g, g.r16());
		else
		    cc_.movsx(g, g.r16());
	    }
	    else if ( src.type->size == 1 )
	    {
		if ( is_unsigned )
		    cc_.movzx(g, g.r8());
		else
		    cc_.movsx(g, g.r8());
	    }
	}
	return src;
    }

    // Mem: size-aware load with sign/zero extension for narrow ints.
    if ( src.isMem() )
    {
	IRValue dst = newReg(src.type, "ir_load");
	x86::Mem m = src.op.as<x86::Mem>();
	uint32_t msz = m.size();
	if ( msz == 0 ) msz = (uint32_t)src.type->size;
	// Clamp aggregate sizes (struct/zmmword tags from subscript paths)
	// down to a scalar load width — load reads at most a Gp's worth.
	if ( msz > 8 ) msz = 8;
	// asmjit's encoder requires the Mem operand to carry an explicit
	// size; otherwise it emits InvalidOperandSize. Sync the local Mem
	// to the resolved msz before any cc.mov.
	if ( m.size() != msz ) m.setSize(msz);
	if ( src.type->is_real() )
	{
	    // Real members come in float (4-byte) or double (8-byte)
	    // flavors. The instruction choice must match the Mem's
	    // actual width, not the DataDef's conceptual width — Mem
	    // size is the x86 ground truth.
	    if ( msz == sizeof(float) )
		cc_.movss(dst.op.as<x86::Xmm>(), m);
	    else
		cc_.movsd(dst.op.as<x86::Xmm>(), m);
	    return dst;
	}
	// Integer or pointer. Widening to Gpq happens via the right
	// extend instruction based on Mem size and source signedness.
	x86::Gp g = dst.op.as<x86::Gp>();
	bool is_unsigned = src.type->is_unsigned();
	if ( msz == 8 )
	{
	    cc_.mov(g, m);
	}
	else if ( msz == 4 )
	{
	    if ( g.isGpd() )
		cc_.mov(g, m);           // Gpd: natural 32-bit load
	    else if ( is_unsigned )
		cc_.mov(g.r32(), m);     // Gpq: zero-extend 32→64
	    else
		cc_.movsxd(g, m);        // Gpq: sign-extend 32→64
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
    // encoding matches. When the source Gp's natural width is narrower
    // than the destination Mem (e.g. dst=qword stack slot, src=sh_int
    // call return in a gpw vreg), widen to r64 first via movsx/movzx
    // so the bare `mov qword[mem], gpw` doesn't reach asmjit's encoder.
    x86::Gp g = reg.op.as<x86::Gp>();
    uint32_t dsz = dst_mem.size();
    uint32_t gsz = g.x86RmSize();
    // Clamp dsz to a scalar move width. A Gp source can carry at most
    // 8 bytes, so a Mem with size > 8 (struct/oword/zmmword tagged via
    // sizeof(struct) in subscript paths) becomes a 64-bit qword store
    // — the natural-width slot for a pointer or integer scalar.
    if ( dsz > 8 )
    {
	dsz = 8;
	dst_mem.setSize(8);
    }
    if ( dsz > gsz && gsz > 0 && gsz < 8 )
    {
	x86::Gp wide = cc_.newGpq("ir_store_wide");
	bool is_unsigned = reg.type && reg.type->is_unsigned();
	if      ( gsz == 4 && !is_unsigned ) cc_.movsxd(wide, g);
	else if ( gsz == 4 )                 cc_.mov(wide.r32(), g);  // implicit zero-ext
	else if ( gsz == 2 && !is_unsigned ) cc_.movsx(wide, g);
	else if ( gsz == 2 )                 cc_.movzx(wide, g);
	else if ( gsz == 1 && !is_unsigned ) cc_.movsx(wide, g);
	else                                 cc_.movzx(wide, g);
	g = wide;
	gsz = 8;
    }
    if      ( dsz == 8 ) cc_.mov(dst_mem, g.r64());
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

    // Coercing to void is a discard — the caller is treating the
    // value as a statement expression with no use. Pass the source
    // through unchanged; the load/store pipeline downstream
    // ignores the result. Without this fast path the type-check
    // ladder below throws on `(unnamed) -> void` for cases where
    // src.type is a synthesized fn-ptr/struct without a name.
    if ( to->type() == DataType::dtVOID )
	return src;

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

    // Integer widening: load src, then widen to Gpq if needed.
    // Exception: signed→unsigned of the same size (e.g. int32→uint32)
    // needs truncation to clear the upper sign-extended bits.
    if ( both_int && to->size >= src.type->size )
    {
	IRValue r = load(src);
	if ( to->size == src.type->size && to->size < 8
	  && !src.type->is_unsigned() && to->is_unsigned() )
	    return canonicalize_narrow_integer_reg(cc_, r, to);
	// Widen Gpd → Gpq when destination is 64-bit.
	if ( to->size > src.type->size && src.type->size == 4
	  && r.isReg() && r.op.as<x86::Gp>().isGpd() )
	{
	    x86::Gp wide = cc_.newGpq("ir_widen");
	    if ( src.type->is_unsigned() )
		cc_.mov(wide.r32(), r.op.as<x86::Gp>());
	    else
		cc_.movsxd(wide, r.op.as<x86::Gp>());
	    return IRValue::reg(wide, to);
	}
	return IRValue::reg(r.op, to);
    }

    // Integer narrowing: truncate to the destination width immediately
    // and sign-/zero-extend back to canonical Gpq form. Otherwise an
    // expression like uint32_t(0xffffffff) + 1 would stay 0x100000000 in
    // a 64-bit vreg until a later store happened to truncate it.
    if ( both_int && to->size < src.type->size )
    {
	IRValue r = load(src);
	return canonicalize_narrow_integer_reg(cc_, r, to);
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
	if ( src.type->is_unsigned() )
	    return coerce_unsigned_int_to_real(cc_, r, to);
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

    // Some host helpers still model function-pointer payloads as raw
    // int64 slots (for example std::for_each's callback trampoline).
    // A function reference is still just an 8-byte address in a Gp, so
    // allow a relabel into a same-width integer destination.
    if ( src.type->is_function() && to->is_integer() && to->size >= src.type->size )
    {
	IRValue r = load(src);
	return IRValue::reg(r.op, to);
    }

    // char* ↔ string transient relabel: ternary branches sometimes
    // mix a string literal (dtSTRING) with a char*-yielding pointer
    // expression (e.g. SMAUG `!CAN_PKILL(victim) ? "&W<Peaceful>" :
    // victim->pcdata->clan->badge`). The merged ternary value is
    // labeled dtSTRING but the actual storage is a Gp char*. For
    // printf-style consumers (the typical use), we just need the
    // pointer value. Keep the underlying Gp, relabel as string.
    // Conversely, a string passed where char* is expected still goes
    // through the existing string_cstr coercion at the call site —
    // this handles the OTHER direction.
    if ( src.type->is_pointer() && to->is_string() )
    {
	IRValue r = load(src);
	return IRValue::reg(r.op, to);
    }

    // 8-byte integer / int64-from-dlsym → string relabel. Functions
    // declared via dlsym fallback (e.g. ctime, strchr) return what
    // madc tracks as int64 because the libc prototype isn't seen at
    // parse time. When the ternary merges such a call with a string
    // literal — `ch->save_time ? ctime(&...) : "no save"` — we want
    // both branches to land in the same 8-byte slot. Relabel.
    if ( src.type->is_integer() && src.type->size == 8 && to->is_string() )
    {
	IRValue r = load(src);
	return IRValue::reg(r.op, to);
    }

    // dtSTRING → char* / pointer-to-char: invoke string_cstr on the
    // std::string operand and yield its underlying char buffer. The
    // ternary merge surface relies on this when a string-literal
    // branch is unified against a char*-yielding branch (SMAUG
    // boards.c:1615 `feof(fp) ? "End" : fread_word(fp)` pattern).
    if ( src.type->is_string() && to->is_pointer() )
    {
	extern const char *string_cstr(void *);
	x86::Gp str_gp = materialize_string_object_ptr(cc_, src);
	InvokeNode *call;
	cc_.invoke(&call, imm(string_cstr), FuncSignature::build<const char *, void *>());
	call->setArg(0, str_gp);
	x86::Gp out_gp = cc_.newIntPtr("ir_str_cstr");
	call->setRet(0, out_gp);
	return IRValue::reg(out_gp, to);
    }

    static char msg[256];
    snprintf(msg, sizeof(msg),
	     "IRBuilder::coerce() unsupported type conversion (src=%s -> dst=%s)",
	     src.type ? src.type->name.c_str() : "(null)",
	     to ? to->name.c_str() : "(null)");
    throw (const char *)msg;
}
