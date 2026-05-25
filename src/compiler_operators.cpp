//////////////////////////////////////////////////////////////////////////
//									//
// Operator compile methods and their helpers: arithmetic, comparison,	//
// logical, bitwise, assignment, cast, ternary, increment/decrement,	//
// complex number arithmetic, SIMD operations, integer precision.	//
//									//
// Extracted from compiler.cpp — Phase A step 3 (file splitting).	//
//									//
//////////////////////////////////////////////////////////////////////////
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <complex>
#include <vector>
#include <queue>
#include <stack>
#include <map>
#include <set>
#include <algorithm>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_ir.h"
#include "compiler_internal.h"

using namespace std;
using namespace asmjit;


// Forward declarations for bitfield helpers
static uint64_t bitfield_mask(size_t width)
{
    if ( width >= 64 )
	return UINT64_MAX;
    if ( width == 0 )
	return 0;
    return (UINT64_C(1) << width) - 1;
}
static void emit_and_u64(Program &pgm, x86::Gp &gp, uint64_t mask, const char *hint)
{
    if ( mask == UINT64_MAX )
	return;
    if ( mask == 0 )
    {
	pgm.cc.xor_(gp, gp);
	return;
    }
    if ( mask <= 0x7fffffffULL )
    {
	pgm.cc.and_(gp, imm((int64_t)mask));
	return;
    }
    x86::Gp mask_gp = pgm.cc.newGpq("%s_mask", hint ? hint : "bf");
    pgm.cc.mov(mask_gp, imm((uint64_t)mask));
    pgm.cc.and_(gp, mask_gp);
}

// --- Operator overload dispatch ---

// Check if the left operand is a class type with the given operator method.
// If so, compile a method call and return true (result in out_operand).
// Called at the top of each TokenOperator::compile() to intercept class ops.
static bool try_class_operator_dispatch(Program &pgm, TokenBase *left, TokenBase *right,
					const char *op_name, regdefp_t &regdp,
					Operand &out_operand)
{
    DataDef *ldd = left ? left->datadef() : NULL;
    if ( !ldd || ldd->basetype() != BaseType::btClass )
	return false;
    DataDefCLASS *ddc = static_cast<DataDefCLASS *>(ldd);
    std::string method_name = op_name;
    auto it = ddc->method_map.find(method_name);
    if ( it == ddc->method_map.end() )
	return false;
    Variable *op_mvar = it->second;
    if ( !op_mvar || !op_mvar->data )
	return false;
    FuncDef *op_func = (FuncDef *)((Method *)op_mvar->data)->returns.type;
    if ( !op_func || !op_func->funcnode )
	return false;

    DBG(pgm.cc.comment(("operator overload: " + method_name).c_str()));

    // Compile the left operand (the object — becomes __this)
    regdefp_t lrdp = {NULL, NULL, NULL};
    Operand &lval = left->compile(pgm, lrdp);

    // Get __this pointer
    x86::Gp this_ptr = pgm.cc.newIntPtr("__op_this");
    if ( lval.isMem() )
	pgm.cc.lea(this_ptr, lval.as<x86::Mem>());
    else
	pgm.cc.mov(this_ptr, lval.as<x86::Gp>());

    // Build function signature
    FuncSignature funcsig;
    funcsig.setCallConvId(CallConvId::kCDecl);
    // Return type from the operator method's FuncDef
    DataDef &ret = op_func->returns;
    if ( ret.is_real() )
	funcsig.setRetT<double>();
    else if ( ret.type() == DataType::dtFLOAT )
	funcsig.setRetT<float>();
    else if ( ret.type() == DataType::dtVOID )
	funcsig.setRetT<void>();
    else
	funcsig.setRetT<int64_t>();
    funcsig.addArgT<void *>(); // __this

    // Compile the right operand (the argument)
    std::vector<Operand> arg_ops;
    if ( right )
    {
	regdefp_t rrdp = {NULL, NULL, NULL};
	Operand &rval = right->compile(pgm, rrdp);
	// Determine arg type from the FuncDef (skip __this at index 0)
	DataDef *arg_type = (op_func->parameters.size() > 1)
	    ? op_func->parameters[1] : NULL;
	add_funcsig_arg(funcsig, arg_type);
	arg_ops.push_back(rval);
    }

    // Emit the invoke
    InvokeNode *call;
    pgm.cc.invoke(&call, op_func->funcnode->label(), funcsig);
    call->setArg(0, this_ptr);

    for ( size_t i = 0; i < arg_ops.size(); ++i )
    {
	if ( arg_ops[i].isReg() )
	{
	    if ( arg_ops[i].as<BaseReg>().isGroup(RegGroup::kVec) )
		call->setArg(i + 1, arg_ops[i].as<x86::Xmm>());
	    else
		call->setArg(i + 1, arg_ops[i].as<x86::Gp>());
	}
	else if ( arg_ops[i].isMem() )
	{
	    x86::Gp tmp = pgm.cc.newIntPtr("__op_arg");
	    pgm.cc.mov(tmp, arg_ops[i].as<x86::Mem>());
	    call->setArg(i + 1, tmp);
	}
    }

    // Capture return value
    if ( op_func->returns.is_numeric() )
    {
	if ( op_func->returns.is_real() )
	{
	    x86::Xmm ret = pgm.cc.newXmm("__op_ret");
	    call->setRet(0, ret);
	    out_operand = ret;
	}
	else
	{
	    x86::Gp ret = pgm.cc.newGpq("__op_ret");
	    call->setRet(0, ret);
	    out_operand = ret;
	}
	regdp.second = &op_func->returns;
    }
    // void return — no result to capture

    return true;
}

// --- Operator types and structures ---

typedef void (Program::*SafeBinOp3)(asmjit::Operand &, asmjit::Operand &, DataDef *, DataDef *);
typedef void (Program::*SafeBitOp2)(asmjit::Operand &, asmjit::Operand &, DataDef *);

struct GeneralBinopCascade {
    Operand *caller_dest;
    bool mirror_to_caller;
};

enum class CmpKind : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
enum class SimdBitKind : uint8_t { And, Or, Xor, Shl, Shr };

x86::Gp emit_bitfield_load(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, const char *hint)
{
    x86::Gp out = emit_bitfield_load_storage(pgm, storage, bf, hint);
    if ( bf.bit_offset )
	pgm.cc.shr(out, imm((uint32_t)bf.bit_offset));
    emit_and_u64(pgm, out, bitfield_mask(bf.bit_width), hint);
    emit_bitfield_sign_extend(pgm, out, bf);
    return out;
}

x86::Gp emit_bitfield_store_reg(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, x86::Gp value, const char *hint)
{
    // Bitfield operations need consistent register widths. Widen Gpd→Gpq
    // to match the Gpq temporaries used for masking and shifting.
    if ( value.isGpd() )
    {
	x86::Gp wide = pgm.cc.newGpq("bf_widen");
	pgm.cc.mov(wide.r32(), value);  // zero-extend 32→64
	value = wide;
    }

    uint64_t value_mask = bitfield_mask(bf.bit_width);
    uint64_t storage_full_mask = bitfield_mask(bf.storage_size * 8);
    uint64_t storage_mask = (bf.bit_width >= 64)
	? UINT64_MAX : ((value_mask << bf.bit_offset) & storage_full_mask);

    x86::Gp bits = pgm.cc.newGpq("%s_bits", hint ? hint : "bf");
    pgm.cc.mov(bits, value);
    emit_and_u64(pgm, bits, value_mask, hint);

    x86::Gp result = pgm.cc.newGpq("%s_result", hint ? hint : "bf");
    pgm.cc.mov(result, bits);
    emit_bitfield_sign_extend(pgm, result, bf);

    if ( bf.bit_offset )
	pgm.cc.shl(bits, imm((uint32_t)bf.bit_offset));

    x86::Gp merged;
    if ( storage_mask == storage_full_mask )
	merged = bits;
    else
    {
	merged = emit_bitfield_load_storage(pgm, storage, bf, hint);
	emit_and_u64(pgm, merged, storage_full_mask & ~storage_mask, hint);
	pgm.cc.or_(merged, bits);
    }
    emit_bitfield_store_storage(pgm, storage, bf, merged);
    return result;
}

x86::Gp emit_bitfield_store_operand(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, Operand &value, DataDef *value_type,
    DataDef *field_type, const char *hint)
{
    IRBuilder ir(pgm.cc);
    DataDef *source_type = value_type ? value_type : field_type;
    IRValue source = ir_from_operand(value, source_type);
    IRValue coerced = ir.coerce(source, field_type);
    IRValue loaded = ir.load(coerced);
    if ( !loaded.op.isReg() || !loaded.op.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "emit_bitfield_store_operand() expected a Gp value";
    return emit_bitfield_store_reg(pgm, storage, bf, loaded.op.as<x86::Gp>(), hint);
}

// LHS resolver shared by inc/dec and compound-assignment operators.
// Handles plain variables and struct members (via -> or .) and *ptr derefs.
// For member/deref, loads the Mem into a Gp and records the Mem for writeback.
struct CompoundLHS {
    Operand lval;        // Gp register holding the current value
    DataDef *type;       // data type of the LHS
    x86::Mem writeback;  // Mem to write back to (for members); invalid if variable
    TokenVar *tv;        // variable token (for modified/putreg); NULL for members
    bool is_member;
    bool is_bitfield;
    DataDefSTRUCT::BitFieldInfo bitfield;

    void finish(Program &pgm)
    {
	if ( tv )
	{
	    tv->var.modified();
	    tv->putreg(pgm);
	}
	if ( is_bitfield && writeback.hasBase() )
	{
	    lval = emit_bitfield_store_reg(pgm, writeback, bitfield,
		lval.as<x86::Gp>(), "cmpd_bf");
	    return;
	}
	if ( is_member && writeback.hasBase() )
	{
	    if ( type && type->is_simd() )
	    {
		x86::Xmm xmm = lval.as<x86::Xmm>();
		store_xmm_to_mem(pgm, writeback, xmm, type);
	    }
	    else
	    {
		IRBuilder ir(pgm.cc);
		ir.store(IRValue::mem(writeback, type), ir_from_operand(lval, type));
	    }
	}
    }
};


typedef void (Program::*SafeUnaryStep)(asmjit::Operand &);

// Shared helpers declared here for use by operator code.
// Definitions remain in compiler.cpp; declarations in compiler_internal.h.

bool is_arithmetic_result_operator(TokenBase *token)
{
    if ( !token || !token->is_operator() )
	return false;
    switch ( token->id() )
    {
	case TokenID::tkAdd:
	case TokenID::tkSub:
	case TokenID::tkMul:
	case TokenID::tkDiv:
	case TokenID::tkMod:
	case TokenID::tkBand:
	case TokenID::tkBor:
	case TokenID::tkXor:
	case TokenID::tkBSL:
	case TokenID::tkBSR:
	    return true;
	default:
	    return false;
    }
}

static DataDef *promoted_bitfield_numeric_type(TokenBase *token)
{
    TokenMember *tm = dynamic_cast<TokenMember *>(token);
    if ( !tm )
	return NULL;
    const DataDefSTRUCT::BitFieldInfo *bf = tm->bitfield_info();
    if ( !bf || bf->bit_width == 0 )
	return NULL;

    size_t int_bits = ddINT.size * 8;
    if ( bf->bit_width < int_bits )
	return &ddINT;
    if ( bf->bit_width == int_bits )
	return bf->is_unsigned ? (DataDef *)&ddUINT32 : (DataDef *)&ddINT;
    return NULL;
}

static IntegerPrecision integer_precision_for_token(TokenBase *token)
{
    if ( !token )
	return IntegerPrecision();
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(token) )
    {
	if ( const DataDefSTRUCT::BitFieldInfo *bf = tm->bitfield_info() )
	    return promoted_bitfield_precision(*bf);
    }
    if ( is_arithmetic_result_operator(token) )
    {
	TokenOperator *op = dynamic_cast<TokenOperator *>(token);
	if ( op && (op->left || op->right) )
	    return binary_integer_precision(op->left, op->right,
		infer_numeric_type(op->left, op->right));
    }
    DataDef *dd = token->datadef();
    if ( dd && dd->is_integer() )
	return IntegerPrecision(dd->size * 8, dd->is_unsigned(), false);
    return IntegerPrecision();
}

IntegerPrecision promoted_bitfield_precision(const DataDefSTRUCT::BitFieldInfo &bf)
{
    if ( bf.bit_width == 0 )
	return IntegerPrecision();

    size_t int_bits = ddINT.size * 8;
    if ( bf.bit_width < int_bits )
	return IntegerPrecision(int_bits, false, true);
    if ( bf.bit_width == int_bits )
	return IntegerPrecision(int_bits, bf.is_unsigned, true);
    return IntegerPrecision(bf.bit_width, bf.is_unsigned, true);
}

IntegerPrecision binary_integer_precision(TokenBase *left, TokenBase *right,
						 DataDef *result_type)
{
    IntegerPrecision lp = integer_precision_for_token(left);
    IntegerPrecision rp = integer_precision_for_token(right);
    if ( !lp.valid && !rp.valid )
	return IntegerPrecision();

    size_t result_bits = (result_type && result_type->is_integer())
	? result_type->size * 8 : 64;
    size_t bits = 0;
    if ( lp.valid && lp.bits > bits )
	bits = lp.bits;
    if ( rp.valid && rp.bits > bits )
	bits = rp.bits;
    if ( bits == 0 )
	bits = result_bits;
    if ( bits > result_bits )
	bits = result_bits;

    bool is_unsigned = result_type ? result_type->is_unsigned()
	: ((lp.valid && lp.is_unsigned) || (rp.valid && rp.is_unsigned));
    return IntegerPrecision(bits, is_unsigned,
	lp.bitfield_derived || rp.bitfield_derived);
}

static void apply_integer_precision(Program &pgm, Operand &value,
				    const IntegerPrecision &precision,
				    DataDef *result_type, const char *hint)
{
    if ( !precision.valid || !precision.bitfield_derived
      || !result_type || !result_type->is_integer() )
	return;
    size_t result_bits = result_type->size * 8;
    if ( precision.bits >= result_bits || precision.bits >= 64 )
	return;
    if ( !value.isReg() || !value.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "apply_integer_precision() expected a Gp result";

    x86::Gp gp = value.as<x86::Gp>();
    if ( precision.is_unsigned )
	emit_and_u64(pgm, gp, bitfield_mask(precision.bits), hint);
    else
    {
	uint32_t shift = (uint32_t)(64 - precision.bits);
	pgm.cc.shl(gp, imm(shift));
	pgm.cc.sar(gp, imm(shift));
    }
}

static void apply_binary_integer_precision(Program &pgm, Operand &value,
					   TokenBase *left, TokenBase *right,
					   DataDef *result_type,
					   bool left_precision_only,
					   const char *hint)
{
    IntegerPrecision precision = left_precision_only
	? integer_precision_for_token(left)
	: binary_integer_precision(left, right, result_type);
    if ( left_precision_only && precision.valid )
	precision.is_unsigned = result_type ? result_type->is_unsigned()
	    : precision.is_unsigned;
    apply_integer_precision(pgm, value, precision, result_type, hint);
}

DataDef *token_numeric_type(TokenBase *token)
{
    if ( !token )
	return NULL;
    if ( DataDef *bf_type = promoted_bitfield_numeric_type(token) )
	return bf_type;
    DataDef *dd = token->datadef();
    if ( dd && dd->is_pointer() )
	return NULL;
    if ( is_arithmetic_result_operator(token) )
    {
	TokenOperator *op = dynamic_cast<TokenOperator *>(token);
	if ( op && (op->left || op->right) )
	    return infer_numeric_type(op->left, op->right);
    }
    if ( dd && dd->is_numeric() )
	return dd;
    return NULL;
}

static uint64_t unsigned_max_for_size(size_t size)
{
    if ( size >= 8 )
	return UINT64_MAX;
    return (uint64_t(1) << (size * 8)) - 1;
}

static bool integer_literal_fits_type(TokenBase *token, DataDef *type)
{
    TokenInt *literal = dynamic_cast<TokenInt *>(token);
    if ( !literal || !type || !type->is_integer() )
	return false;

    int64_t value = literal->get();
    if ( type->is_unsigned() )
	return value >= 0 && (uint64_t)value <= unsigned_max_for_size(type->size);

    if ( type->size >= 8 )
	return true;
    size_t bits = type->size * 8;
    int64_t max_value = (int64_t(1) << (bits - 1)) - 1;
    int64_t min_value = -(int64_t(1) << (bits - 1));
    return value >= min_value && value <= max_value;
}

DataDef *choose_integer_type(TokenBase *left, DataDef *lt,
				    TokenBase *right, DataDef *rt)
{
    if ( !lt )
	return rt;
    if ( !rt )
	return lt;
    if ( lt == rt )
	return lt;

    // Unsuffixed integer literals are currently represented by madc's
    // historical 64-bit ddINT. When paired with an explicitly narrower C
    // type and the value fits, use the explicit type so `uint32 + 1`
    // follows C's 32-bit unsigned wrap semantics.
    if ( integer_literal_fits_type(left, rt) && lt == &ddINT && rt != &ddINT )
	return rt;
    if ( integer_literal_fits_type(right, lt) && rt == &ddINT && lt != &ddINT )
	return lt;

    bool lu = lt->is_unsigned();
    bool ru = rt->is_unsigned();
    if ( lu != ru )
    {
	if ( lt->size == rt->size )
	    return lu ? lt : rt;
	if ( lu && lt->size > rt->size )
	    return lt;
	if ( ru && rt->size > lt->size )
	    return rt;
	return lt->size > rt->size ? lt : rt;
    }

    if ( lt->size != rt->size )
	return lt->size > rt->size ? lt : rt;
    return lt;
}

static int integer_rank(DataDef *type)
{
    if ( !type )
	return 0;
    if ( type->size <= 1 )
	return 1;
    if ( type->size <= 2 )
	return 2;
    if ( type->size <= 3 )
	return 3;
    if ( type->size <= 4 )
	return 4;
    return 5;
}

static DataDef *unsigned_integer_type_for_size(size_t size)
{
    if ( size <= 1 ) return &ddUINT8;
    if ( size <= 2 ) return &ddUINT16;
    if ( size <= 3 ) return &ddUINT24;
    if ( size <= 4 ) return &ddUINT32;
    return &ddUINT64;
}

static DataDef *promoted_shift_result_type(TokenBase *left)
{
    DataDef *left_type = token_numeric_type(left);
    if ( !left_type || !left_type->is_integer() )
	return left_type;
    return promote_c_integer_type(left_type);
}

static bool signed_type_can_represent_unsigned(DataDef *signed_type, DataDef *unsigned_type)
{
    return signed_type && unsigned_type
	&& !signed_type->is_unsigned()
	&& unsigned_type->is_unsigned()
	&& signed_type->size > unsigned_type->size;
}

DataDef *usual_binary_integer_type(TokenBase *left, DataDef *lt,
					  TokenBase *right, DataDef *rt)
{
    if ( !lt || !rt )
	return choose_integer_type(left, lt, right, rt);

    // madc's unsuffixed integer literals historically parse as 64-bit
    // ddINT. When paired with an explicit C int/long-width operand and
    // the value fits, treat the literal as that C type before promotions.
    if ( integer_literal_fits_type(left, rt) && lt == &ddINT && rt != &ddINT && rt->size >= 4 )
	lt = rt;
    if ( integer_literal_fits_type(right, lt) && rt == &ddINT && lt != &ddINT && lt->size >= 4 )
	rt = lt;

    lt = promote_c_integer_type(lt);
    rt = promote_c_integer_type(rt);
    if ( lt == rt )
	return lt;

    bool lu = lt->is_unsigned();
    bool ru = rt->is_unsigned();
    int lr = integer_rank(lt);
    int rr = integer_rank(rt);

    if ( lu == ru )
	return lr >= rr ? lt : rt;

    DataDef *ut = lu ? lt : rt;
    DataDef *st = lu ? rt : lt;
    int ur = lu ? lr : rr;
    int sr = lu ? rr : lr;

    if ( ur >= sr )
	return ut;
    if ( signed_type_can_represent_unsigned(st, ut) )
	return st;
    return unsigned_integer_type_for_size(st->size);
}

static bool should_use_natural_divmod_type(DataDef *requested, DataDef *natural)
{
    if ( !requested || !natural )
	return false;
    if ( !requested->is_integer() || !natural->is_integer() )
	return false;
    if ( requested == natural )
	return false;
    if ( requested->size < natural->size )
	return true;
    return natural->is_unsigned()
	&& !requested->is_unsigned()
	&& requested->size <= natural->size;
}

static void splat_gp_to_simd_xmm(Program &pgm, x86::Xmm &dst, x86::Gp gp,
				 DataDefSIMD *vdd)
{
    if ( !vdd || !vdd->element_type || vdd->element_type->size == 0 )
	throw "splat_gp_to_simd_xmm() invalid SIMD type";
    uint32_t bytes = (uint32_t)vdd->size;
    x86::Mem slot = pgm.cc.newStack(bytes, (uint32_t)vdd->alignment());
    slot.setSize(bytes);
    size_t elem_size = vdd->element_type->size;
    for ( size_t i = 0; i < vdd->lane_count; ++i )
    {
	x86::Mem lane = slot;
	lane.addOffset((int64_t)(i * elem_size));
	lane.setSize((uint32_t)elem_size);
	if ( elem_size == 1 )
	    pgm.cc.mov(lane, gp.r8());
	else if ( elem_size == 2 )
	    pgm.cc.mov(lane, gp.r16());
	else if ( elem_size == 4 )
	    pgm.cc.mov(lane, gp.r32());
	else
	    pgm.cc.mov(lane, gp.r64());
    }
    load_mem_to_xmm(pgm, dst, slot, vdd);
}

// Broadcast a scalar Xmm (value in low lane) to all lanes of dst.
void splat_xmm_to_simd_xmm(Program &pgm, x86::Xmm &dst, x86::Xmm src,
				   DataDefSIMD *vdd)
{
    if ( !vdd || !vdd->element_type || vdd->element_type->size == 0 )
	throw "splat_xmm_to_simd_xmm() invalid SIMD type";
    // For float (4 lanes): shufps dst, dst, 0 broadcasts lane 0 to all
    // For double (2 lanes): unpcklpd dst, dst broadcasts lane 0 to both
    if ( !dst.equals(src) )
	pgm.cc.emit(asmjit::x86::Inst::kIdMovaps, dst, src);
    if ( vdd->element_type->size == sizeof(float) )
	pgm.cc.shufps(dst, dst, 0);
    else if ( vdd->element_type->size == sizeof(double) )
	pgm.cc.unpcklpd(dst, dst);
    else
	throw "splat_xmm_to_simd_xmm() non-float element type";
}

// Splat a scalar IRValue (Gp or Xmm) into all lanes of a SIMD Xmm register.
void splat_scalar_to_simd(Program &pgm, x86::Xmm &dst,
				 const IRValue &scalar, DataDefSIMD *vdd)
{
    if ( scalar.op.isReg() && scalar.op.as<BaseReg>().isGroup(RegGroup::kGp) )
	splat_gp_to_simd_xmm(pgm, dst, scalar.op.as<x86::Gp>(), vdd);
    else if ( scalar.op.isReg() && scalar.op.as<BaseReg>().isGroup(RegGroup::kVec) )
	splat_xmm_to_simd_xmm(pgm, dst, scalar.op.as<x86::Xmm>(), vdd);
    else
	throw "splat_scalar_to_simd() unexpected operand type";
}

static Operand &compile_compound_rhs_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
						Operand &storage)
{
    return compile_token_normalized(pgm, token, target_type, nullptr, storage);
}

static Operand &compile_compound_rhs_gp_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
						   Operand &storage)
{
    return compile_token_gp_normalized(pgm, token, target_type, storage);
}

// Normalize both sides through the IR to Reg-of-result_type, run the
// given safe* helper in place on the left Reg, then route the result
// through emit_ir_value so a caller's regdp.first (Reg or Mem) is
// honored without every token duplicating the dest-dance.
static Operand &emit_plain_binop3(Program &pgm, TokenBase *left, TokenBase *right,
				  DataDef *result_type, SafeBinOp3 op,
				  regdefp_t &regdp, Operand &storage, const char *name_hint)
{
    Operand *caller_dest = regdp.first;
    Operand left_reg = result_type->newreg(pgm.cc, name_hint);
    Operand left_norm;
    Operand right_norm;
    Operand &lval = compile_token_normalized(pgm, left, result_type, &left_reg, left_norm);
    Operand &rval = compile_token_normalized(pgm, right, result_type, nullptr, right_norm);
    (pgm.*op)(lval, rval, result_type, nullptr);
    apply_binary_integer_precision(pgm, lval, left, right, result_type,
	false, name_hint);
    IRBuilder ir(pgm.cc);
    IRValue result = ir.load(IRValue::reg(lval, result_type));
    storage = result.op;
    regdp.first = &storage;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(storage, result_type), regdp, storage, result_type);
    }
    return storage;
}

// Bitwise/shift version: safe* helper takes 2 args only, and (for
// shifts) the right side is forced to a Gp through
// compile_token_gp_normalized — matching the original per-op code.
static Operand &emit_plain_bitop2(Program &pgm, TokenBase *left, TokenBase *right,
				  DataDef *result_type, SafeBitOp2 op,
				  bool right_must_be_gp,
				  bool left_precision_only,
				  regdefp_t &regdp, Operand &storage, const char *name_hint)
{
    Operand *caller_dest = regdp.first;
    Operand left_reg = result_type->newreg(pgm.cc, name_hint);
    Operand left_norm;
    Operand right_norm;
    Operand &lval = compile_token_normalized(pgm, left, result_type, &left_reg, left_norm);
    Operand &rval = right_must_be_gp
	? compile_token_gp_normalized(pgm, right, result_type, right_norm)
	: compile_token_normalized(pgm, right, result_type, nullptr, right_norm);
    (pgm.*op)(lval, rval, result_type);
    apply_binary_integer_precision(pgm, lval, left, right, result_type,
	left_precision_only, name_hint);
    IRBuilder ir(pgm.cc);
    IRValue result = ir.load(IRValue::reg(lval, result_type));
    storage = result.op;
    regdp.first = &storage;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(storage, result_type), regdp, storage, result_type);
    }
    return storage;
}

// Similar to emit_plain_binop3 but for safediv, which takes a separate
// remainder register and an extra pair of type slots. return_remainder
// selects TokenMod semantics; otherwise we return the dividend (TokenDiv).
static Operand &emit_plain_divmod(Program &pgm, TokenBase *left, TokenBase *right,
				  DataDef *result_type, bool return_remainder,
				  regdefp_t &regdp, Operand &storage, const char *name_hint)
{
    Operand *caller_dest = regdp.first;
    Operand dividend_reg = result_type->newreg(pgm.cc, name_hint);
    Operand remainder    = result_type->newreg(pgm.cc, "_divmod_rem");
    Operand left_norm;
    Operand right_norm;
    Operand &dividend = compile_token_normalized(pgm, left,  result_type, &dividend_reg, left_norm);
    Operand &divisor  = compile_token_normalized(pgm, right, result_type, nullptr,       right_norm);
    if ( result_type->is_integer() )
	pgm.safexor(remainder, remainder);
    pgm.safediv(remainder, dividend, divisor, result_type, result_type, result_type);
    Operand &result = return_remainder ? remainder : dividend;
    apply_binary_integer_precision(pgm, result, left, right, result_type,
	false, name_hint);
    IRBuilder ir(pgm.cc);
    IRValue normalized = ir.load(IRValue::reg(result, result_type));
    storage = normalized.op;
    regdp.first = &storage;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(storage, result_type), regdp, storage, result_type);
    }
    return storage;
}

DataDef *effective_pointer_type_for_arith(Program &pgm, TokenBase *tb)
{
    if ( !tb )
	return nullptr;
    if ( token_has_constant_cstring(tb) )
	return &ddLPSTR;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) )
	if ( tv->var.is_fixed_array() && tv->var.type )
	    return pgm.getPointerType(tv->var.type);
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(tb) )
	if ( tm->is_fixed_array_member() && tm->var.type )
	    return pgm.getPointerType(tm->var.type);
    DataDef *dd = tb->datadef();
    if ( dd && dd->is_pointer() )
	return dd;
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(dd) )
    {
	DataDef *elem = add->element_type ? add->element_type : &ddINT64;
	return pgm.getPointerType(elem);
    }
    if ( TokenOperator *op = dynamic_cast<TokenOperator *>(tb) )
    {
	DataDef *lptr = effective_pointer_type_for_arith(pgm, op->left);
	DataDef *rptr = effective_pointer_type_for_arith(pgm, op->right);
	if ( op->id() == TokenID::tkAdd || op->id() == TokenID::tkSub )
	    return lptr ? lptr : rptr;
    }
    return nullptr;
}


static void emit_pointer_arith_scale(Program &pgm, TokenBase *left, TokenBase *right,
				     Operand &rval)
{
    DataDef *rtype = effective_pointer_type_for_arith(pgm, right);
    if ( rtype )
	return;  // ptr ± ptr: raw byte arithmetic, no scale
    size_t elem_size = 0;
    DataDef *ltype = effective_pointer_type_for_arith(pgm, left);
    if ( ltype )
    {
	DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(ltype);
	if ( pdd && pdd->base_type )
	    elem_size = pdd->base_type->size;
    }
    if ( elem_size <= 1 )
	return;
    if ( !rval.isReg() || !rval.as<BaseReg>().isGroup(RegGroup::kGp) )
	return;
    pgm.cc.imul(rval.as<x86::Gp>(), rval.as<x86::Gp>(), imm((int64_t)elem_size));
}

static Operand &compile_pointer_offset_operand(Program &pgm, TokenBase *offset_expr,
					       Operand &storage, DataDef *&offset_type)
{
    regdefp_t rhs_rdp = {nullptr, nullptr, nullptr};
    Operand &raw = offset_expr->compile(pgm, rhs_rdp);
    offset_type = rhs_rdp.second
	? rhs_rdp.second
	: (offset_expr && offset_expr->datadef() ? offset_expr->datadef() : &ddINT);
    IRBuilder ir(pgm.cc);
    IRValue widened = ir.coerce(ir_from_operand(raw, offset_type), &ddINT64);
    widened = ir.load(widened);
    storage = widened.op;
    offset_type = &ddINT64;
    return storage;
}

Operand &compile_complex_compare_base(Program &pgm, TokenBase *expr, DataDef *expr_type,
					     Operand &storage)
{
    regdefp_t sub = {nullptr, expr_type, nullptr};
    Operand &base = expr->compile(pgm, sub);
    storage = base;
    return storage;
}

x86::Mem complex_component_mem(Program &pgm, const Operand &base_op,
				      DataDefCOMPLEX *cdd, bool imag_part,
				      const char *tmp_name)
{
    DataDef *part_type = cdd && cdd->element_type ? cdd->element_type : &ddINT64;
    size_t ofs = cdd ? cdd->component_offset(imag_part) : 0;
    if ( base_op.isMem() )
    {
	x86::Mem part = base_op.as<x86::Mem>();
	part.addOffset((int64_t)ofs);
	part.setSize((uint32_t)part_type->size);
	return part;
    }

    x86::Gp base_gp;
    if ( base_op.isImm() )
    {
	base_gp = pgm.cc.newIntPtr(tmp_name);
	pgm.cc.mov(base_gp, base_op.as<Imm>());
    }
    else
	base_gp = base_op.as<x86::Gp>();
    return x86::ptr(base_gp, (int32_t)ofs, (uint32_t)part_type->size);
}

DataDef *complex_component_type(DataDef *dd)
{
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(dd);
    return cdd && cdd->element_type ? cdd->element_type : dd;
}


static x86::Gp emit_complex_cmp_bit(Program &pgm, const Operand &lhs, DataDef *lhs_type,
				    const Operand &rhs, DataDef *rhs_type,
				    DataDef *cmp_type, CmpKind op,
				    const char *name)
{
    IRBuilder ir(pgm.cc);
    IRValue left_val = ir.load(ir.coerce(ir_from_operand(lhs, lhs_type), cmp_type));
    IRValue right_val = ir.load(ir.coerce(ir_from_operand(rhs, rhs_type), cmp_type));
    x86::Gp result = pgm.cc.newGpq(name);
    pgm.safecmp(left_val.op, right_val.op, cmp_type);
    if ( op == CmpKind::Eq )
	pgm.safesete(result);
    else
	pgm.safesetne(result);
    return result;
}

static Operand &emit_complex_compare(Program &pgm, TokenBase *left, TokenBase *right, CmpKind op,
				     regdefp_t &regdp, Operand &storage)
{
    Operand *caller_dest = regdp.first;
    DataDef *left_type = left ? left->datadef() : nullptr;
    DataDef *right_type = right ? right->datadef() : nullptr;
    bool left_complex = left_type && left_type->is_complex();
    bool right_complex = right_type && right_type->is_complex();
    DataDef *component_type = complex_component_type(left_complex ? left_type : right_type);
    if ( !component_type )
	component_type = &ddINT;

    Operand left_base;
    Operand right_base;
    Operand left_real_storage;
    Operand left_imag_storage;
    Operand right_real_storage;
    Operand right_imag_storage;
    Operand left_scalar_storage;
    Operand left_zero_storage;
    Operand right_scalar_storage;
    Operand right_zero_storage;
    TokenInt zero(0);

    Operand *left_real = nullptr;
    Operand *left_imag = nullptr;
    Operand *right_real = nullptr;
    Operand *right_imag = nullptr;
    DataDef *left_real_type = component_type;
    DataDef *left_imag_type = component_type;
    DataDef *right_real_type = component_type;
    DataDef *right_imag_type = component_type;

    if ( left_complex )
    {
	DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(left_type);
	if ( cdd && cdd->element_type )
	    left_real_type = left_imag_type = cdd->element_type;
	Operand &base = compile_complex_compare_base(pgm, left, left_type, left_base);
	left_real_storage = complex_component_mem(pgm, base, cdd, false, "__complex_cmp_l");
	left_imag_storage = complex_component_mem(pgm, base, cdd, true, "__complex_cmp_l");
	left_real = &left_real_storage;
	left_imag = &left_imag_storage;
    }
    else
    {
	left_real = &compile_token_normalized(pgm, left, component_type, nullptr, left_scalar_storage);
	left_real_type = component_type;
	left_imag = &compile_token_normalized(pgm, &zero, component_type, nullptr, left_zero_storage);
	left_imag_type = component_type;
    }

    if ( right_complex )
    {
	DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(right_type);
	if ( cdd && cdd->element_type )
	    right_real_type = right_imag_type = cdd->element_type;
	Operand &base = compile_complex_compare_base(pgm, right, right_type, right_base);
	right_real_storage = complex_component_mem(pgm, base, cdd, false, "__complex_cmp_r");
	right_imag_storage = complex_component_mem(pgm, base, cdd, true, "__complex_cmp_r");
	right_real = &right_real_storage;
	right_imag = &right_imag_storage;
    }
    else
    {
	right_real = &compile_token_normalized(pgm, right, component_type, nullptr, right_scalar_storage);
	right_real_type = component_type;
	right_imag = &compile_token_normalized(pgm, &zero, component_type, nullptr, right_zero_storage);
	right_imag_type = component_type;
    }

    x86::Gp real_cmp = emit_complex_cmp_bit(pgm, *left_real, left_real_type,
					    *right_real, right_real_type,
					    component_type, op, "__complex_cmp_re");
    x86::Gp imag_cmp = emit_complex_cmp_bit(pgm, *left_imag, left_imag_type,
					    *right_imag, right_imag_type,
					    component_type, op, "__complex_cmp_im");
    if ( op == CmpKind::Eq )
	pgm.safeand(real_cmp, imag_cmp, &ddINT64);
    else
	pgm.safeor(real_cmp, imag_cmp, &ddINT64);

    regdp.second = &ddINT64;
    storage = real_cmp;
    regdp.first = &storage;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(real_cmp, &ddINT64), regdp, storage, &ddINT64);
    }
    return storage;
}

static DataDefSIMD *compare_simd_type(TokenBase *left, TokenBase *right, DataDef *requested_type)
{
    if ( requested_type && requested_type->is_simd() )
	return static_cast<DataDefSIMD *>(requested_type);
    if ( left && left->datadef() && left->datadef()->is_simd() )
	return static_cast<DataDefSIMD *>(left->datadef());
    if ( right && right->datadef() && right->datadef()->is_simd() )
	return static_cast<DataDefSIMD *>(right->datadef());
    return NULL;
}

static x86::Xmm emit_simd_all_ones(Program &pgm, const char *name)
{
    x86::Xmm all_ones = pgm.cc.newXmm(name);
    pgm.cc.pxor(all_ones, all_ones);
    pgm.cc.pcmpeqb(all_ones, all_ones);
    return all_ones;
}

static void emit_simd_integer_eq_mask(Program &pgm, x86::Xmm &dst, x86::Xmm &rhs, DataDefSIMD *vdd)
{
    size_t elem_size = (vdd && vdd->element_type) ? vdd->element_type->size : 0;
    if ( elem_size == 1 )
    {
	pgm.cc.pcmpeqb(dst, rhs);
	return;
    }
    if ( elem_size == 2 )
    {
	pgm.cc.pcmpeqw(dst, rhs);
	return;
    }
    if ( elem_size == 4 )
    {
	pgm.cc.pcmpeqd(dst, rhs);
	return;
    }
    if ( elem_size == 8 )
    {
	pgm.cc.pcmpeqd(dst, rhs);
	x86::Xmm paired = pgm.cc.newXmm("simd_eq64_pair");
	pgm.cc.movdqa(paired, dst);
	pgm.cc.pshufd(paired, paired, 0xB1);
	pgm.cc.pand(dst, paired);
	return;
    }
    throw "emit_simd_integer_eq_mask() unsupported SIMD element size";
}

static Operand &emit_simd_compare(Program &pgm, TokenBase *left, TokenBase *right, CmpKind op,
				  DataDefSIMD *simd_type, regdefp_t &regdp, Operand &storage)
{
    if ( !simd_type || !simd_type->element_type )
	throw "emit_simd_compare() invalid SIMD compare type";

    Operand *caller_dest = regdp.first;
    Operand left_storage = pgm.cc.newXmm("simd_cmp_l");
    Operand right_storage = pgm.cc.newXmm("simd_cmp_r");
    Operand &left_val = compile_token_normalized(pgm, left, simd_type, &left_storage, left_storage);
    Operand &right_val = compile_token_normalized(pgm, right, simd_type, &right_storage, right_storage);
    x86::Xmm result = pgm.cc.newXmm("simd_cmp");
    pgm.cc.movdqa(result, left_val.as<x86::Xmm>());

    if ( simd_type->element_type->is_real() )
    {
	if ( simd_type->element_type->size == sizeof(float) )
	    pgm.cc.cmpps(result, right_val.as<x86::Xmm>(), x86::CmpImm::kEQ);
	else
	    pgm.cc.cmppd(result, right_val.as<x86::Xmm>(), x86::CmpImm::kEQ);
    }
    else
	emit_simd_integer_eq_mask(pgm, result, right_val.as<x86::Xmm>(), simd_type);

    if ( op == CmpKind::Ne )
    {
	x86::Xmm all_ones = emit_simd_all_ones(pgm, "simd_cmp_not");
	pgm.cc.pxor(result, all_ones);
    }

    regdp.second = simd_type;
    storage = result;
    regdp.first = &storage;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(result, simd_type), regdp, storage, simd_type);
    }
    return storage;
}

static bool expr_contains_simd_value(TokenBase *expr)
{
    if ( !expr )
	return false;
    if ( expr->datadef() && expr->datadef()->is_simd() )
	return true;
    if ( TokenOperator *op = dynamic_cast<TokenOperator *>(expr) )
	return expr_contains_simd_value(op->left)
	    || expr_contains_simd_value(op->right);
    return false;
}

static x86::Mem sized_simd_mem(const x86::Mem &mem, DataDefSIMD *vdd)
{
    x86::Mem out = mem;
    out.setSize((uint32_t)(vdd ? vdd->size : 16));
    return out;
}

x86::Mem large_simd_expr_mem(Program &pgm, TokenBase *expr, DataDefSIMD *vdd,
				    const char *name)
{
    if ( !expr || !vdd )
	throw "large SIMD expression missing type";

    if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
    {
	Operand &op = tv->operand(pgm);
	if ( op.isMem() )
	    return sized_simd_mem(op.as<x86::Mem>(), vdd);
	if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
	    return x86::ptr(op.as<x86::Gp>(), 0, (uint32_t)vdd->size);
    }

    if ( TokenStructLit *slit = dynamic_cast<TokenStructLit *>(expr) )
    {
	if ( slit->datadef() && slit->datadef()->is_simd() )
	{
	    x86::Mem mem = new_large_simd_stack(pgm, vdd, name);
	    x86::Gp base = pgm.cc.newIntPtr("%s_base", name ? name : "simd_lit");
	    pgm.cc.lea(base, mem);
	    emit_zero_fill_region(pgm, base, 0, vdd->size);
	    emit_simd_init(pgm, base, 0, vdd, slit->inits, expr);
	    return mem;
	}
    }

    regdefp_t rdp = {nullptr, vdd, nullptr};
    Operand &op = expr->compile(pgm, rdp);
    if ( op.isMem() )
	return sized_simd_mem(op.as<x86::Mem>(), vdd);
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
	return x86::ptr(op.as<x86::Gp>(), 0, (uint32_t)vdd->size);
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	x86::Mem mem = new_large_simd_stack(pgm, vdd, name);
	x86::Xmm xmm = op.as<x86::Xmm>();
	store_xmm_to_mem(pgm, mem, xmm, vdd);
	return mem;
    }
    throw "large SIMD expression did not materialize as memory";
}

static Operand &finish_simd_mem_result(Program &pgm, x86::Mem &src_mem,
				       DataDefSIMD *vdd, regdefp_t &regdp,
				       Operand &storage, const char *copy_name)
{
    regdp.second = vdd;
    if ( Operand *caller_dest = regdp.first )
    {
	if ( caller_dest->isReg()
	  && caller_dest->as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    x86::Xmm dst = caller_dest->as<x86::Xmm>();
	    load_mem_to_xmm(pgm, dst, src_mem, vdd);
	    return *caller_dest;
	}
	Operand src = src_mem;
	emit_raw_aggregate_copy(pgm, *caller_dest, src, vdd, copy_name);
	regdp.first = caller_dest;
	return *caller_dest;
    }
    storage = src_mem;
    regdp.first = &storage;
    return storage;
}

static Operand &emit_large_simd_compare(Program &pgm, TokenBase *left, TokenBase *right,
					CmpKind op, DataDefSIMD *vdd,
					regdefp_t &regdp, Operand &storage)
{
    if ( !vdd || !vdd->element_type || !vdd->element_type->is_integer() )
	throw "large SIMD compare currently supports integer vectors";

    DataDef *elem = vdd->element_type;
    bool is_unsigned = elem->is_unsigned();
    bool left_vec = expr_contains_simd_value(left);
    bool right_vec = expr_contains_simd_value(right);
    x86::Mem left_mem, right_mem;
    Operand left_scalar, right_scalar;
    if ( left_vec )
	left_mem = large_simd_expr_mem(pgm, left, vdd, "simd_cmp_l");
    else
	left_scalar = compile_token_normalized(pgm, left, elem, nullptr, left_scalar);
    if ( right_vec )
	right_mem = large_simd_expr_mem(pgm, right, vdd, "simd_cmp_r");
    else
	right_scalar = compile_token_normalized(pgm, right, elem, nullptr, right_scalar);

    x86::Mem dst = new_large_simd_stack(pgm, vdd, "simd_cmp");
    for ( size_t i = 0; i < vdd->lane_count; ++i )
    {
	Operand ltmp;
	Operand rtmp;
	Operand &lval = left_vec
	    ? (ltmp = load_simd_lane_gp(pgm, left_mem, i, vdd, "simd_cmp_l"))
	    : left_scalar;
	Operand &rval = right_vec
	    ? (rtmp = load_simd_lane_gp(pgm, right_mem, i, vdd, "simd_cmp_r"))
	    : right_scalar;
	x86::Gp bit = pgm.cc.newGpq("simd_cmp_bit");
	pgm.safecmp(lval, rval, elem);
	switch(op)
	{
	    case CmpKind::Eq: pgm.safesete(bit); break;
	    case CmpKind::Ne: pgm.safesetne(bit); break;
	    case CmpKind::Lt: if ( is_unsigned ) pgm.safesetb(bit);  else pgm.safesetl(bit);  break;
	    case CmpKind::Le: if ( is_unsigned ) pgm.safesetbe(bit); else pgm.safesetle(bit); break;
	    case CmpKind::Gt: if ( is_unsigned ) pgm.safeseta(bit);  else pgm.safesetg(bit);  break;
	    case CmpKind::Ge: if ( is_unsigned ) pgm.safesetae(bit); else pgm.safesetge(bit); break;
	}
	pgm.cc.neg(bit);
	store_simd_lane_gp(pgm, dst, i, vdd, bit);
    }

    return finish_simd_mem_result(pgm, dst, vdd, regdp, storage, "simd_cmp_copy");
}

static Operand &emit_large_simd_bitwise(Program &pgm, TokenBase *left, TokenBase *right,
					SimdBitKind op, DataDefSIMD *vdd,
					regdefp_t &regdp, Operand &storage)
{
    if ( !vdd || !vdd->element_type || !vdd->element_type->is_integer() )
	throw "large SIMD bitwise currently supports integer vectors";
    DataDef *elem = vdd->element_type;
    bool left_vec = expr_contains_simd_value(left);
    bool right_vec = expr_contains_simd_value(right);
    x86::Mem left_mem;
    x86::Mem right_mem;
    Operand left_scalar;
    Operand right_scalar;
    if ( left_vec )
	left_mem = large_simd_expr_mem(pgm, left, vdd, "simd_bit_l");
    else
	left_scalar = compile_token_normalized(pgm, left, elem, nullptr, left_scalar);
    if ( right_vec )
	right_mem = large_simd_expr_mem(pgm, right, vdd, "simd_bit_r");
    else
	right_scalar = compile_token_normalized(pgm, right, elem, nullptr, right_scalar);

    x86::Mem dst = new_large_simd_stack(pgm, vdd, "simd_bit");
    for ( size_t i = 0; i < vdd->lane_count; ++i )
    {
	Operand ltmp_storage;
	Operand &ltmp = left_vec
	    ? (ltmp_storage = load_simd_lane_gp(pgm, left_mem, i, vdd, "simd_bit_l"))
	    : left_scalar;
	// For scalar left, we need a fresh copy per lane since the op is destructive
	Operand lval;
	if ( !left_vec )
	{
	    lval = elem->newreg(pgm.cc, "simd_bit_l_copy");
	    pgm.safemov(lval, ltmp, elem, elem);
	}
	else
	    lval = ltmp;
	Operand rtmp;
	Operand &rval = right_vec
	    ? (rtmp = load_simd_lane_gp(pgm, right_mem, i, vdd, "simd_bit_r"))
	    : right_scalar;
	if ( op == SimdBitKind::And )
	    pgm.safeand(lval, rval, elem);
	else if ( op == SimdBitKind::Or )
	    pgm.safeor(lval, rval, elem);
	else if ( op == SimdBitKind::Xor )
	    pgm.safexor(lval, rval, elem);
	else if ( op == SimdBitKind::Shl )
	    pgm.safeshl(lval, rval, elem);
	else
	    pgm.safeshr(lval, rval, elem);
	store_simd_lane_gp(pgm, dst, i, vdd, lval.as<x86::Gp>());
    }

    return finish_simd_mem_result(pgm, dst, vdd, regdp, storage, "simd_bit_copy");
}

static Operand &emit_simd_bitwise_not(Program &pgm, TokenBase *expr, DataDefSIMD *vdd,
				      regdefp_t &regdp, Operand &storage)
{
    if ( !vdd || !vdd->element_type || !vdd->element_type->is_integer() )
	throw "SIMD bitwise-not currently supports integer vectors";
    x86::Mem src_mem = large_simd_expr_mem(pgm, expr, vdd, "simd_not_src");
    x86::Mem dst = new_large_simd_stack(pgm, vdd, "simd_not");
    for ( size_t i = 0; i < vdd->lane_count; ++i )
    {
	x86::Gp lane = load_simd_lane_gp(pgm, src_mem, i, vdd, "simd_not_lane");
	pgm.safenot(lane);
	store_simd_lane_gp(pgm, dst, i, vdd, lane);
    }
    return finish_simd_mem_result(pgm, dst, vdd, regdp, storage, "simd_not_copy");
}

// Central implementation of `lval <cmp> rval` -> 0/1 int64 result.
// Both sides normalize through compile_token_normalized to a Reg of the
// inferred cmp_type; safecmp + the right safeset produce the flag and
// zero-extended byte; the final 0/1 routes through emit_ir_value so a
// caller's regdp.first (Reg or Mem) is honored uniformly.
static Operand &emit_compare(Program &pgm, TokenBase *left, TokenBase *right, CmpKind op,
			     regdefp_t &regdp, Operand &storage)
{
    Operand *caller_dest = regdp.first;
    DataDefSIMD *simd_type = compare_simd_type(left, right, regdp.second);
    if ( simd_type && simd_type->element_type && simd_type->element_type->is_integer() )
	return emit_large_simd_compare(pgm, left, right, op, simd_type, regdp, storage);
    if ( simd_type && (op == CmpKind::Eq || op == CmpKind::Ne) )
	return emit_simd_compare(pgm, left, right, op, simd_type, regdp, storage);
    DataDef *lptr_type = effective_pointer_type_for_arith(pgm, left);
    DataDef *rptr_type = effective_pointer_type_for_arith(pgm, right);
    if ( !lptr_type && !rptr_type
      && (op == CmpKind::Eq || op == CmpKind::Ne)
      && ((left && left->datadef() && left->datadef()->is_complex())
       || (right && right->datadef() && right->datadef()->is_complex())) )
	return emit_complex_compare(pgm, left, right, op, regdp, storage);
    DataDef *cmp_type = (lptr_type || rptr_type)
	? (lptr_type ? lptr_type : rptr_type)
	: infer_numeric_type(left, right);
    // Real comparisons use ucomisd which sets CF/PF/ZF, not SF/OF,
    // so setl/setle/setg/setge are meaningless after ucomisd. The
    // x86 manual mandates the unsigned setcc variants (setb/setbe/
    // seta/setae) to read its flags correctly.
    bool is_unsigned = false;
    if ( op != CmpKind::Eq && op != CmpKind::Ne )
    {
	// C usual arithmetic conversions: unsigned char/short promote to
	// (signed) int, so only unsigned types >= sizeof(int) force
	// unsigned comparison.  Floats use ucomisd which sets CF/PF/ZF,
	// requiring unsigned setcc variants (setb/seta).
	auto forces_unsigned = [](DataDef *dd) {
	    return dd && dd->is_unsigned() && dd->size >= 4;
	};
	is_unsigned = forces_unsigned(left->datadef())
		   || forces_unsigned(right->datadef())
		   || (cmp_type && cmp_type->is_unsigned() && cmp_type->size >= 4)
		   || (cmp_type && cmp_type->is_real());
    }

    Operand left_norm;
    Operand right_norm;
    Operand &lval = compile_token_normalized(pgm, left, cmp_type, nullptr, left_norm);
    Operand &rval = compile_token_normalized(pgm, right, cmp_type, nullptr, right_norm);
    // C usual arithmetic conversions: when comparing int with unsigned int,
    // both convert to unsigned int (32-bit). Truncate 64-bit register values
    // to 32 bits so -1 (0xFFFFFFFFFFFFFFFF) becomes UINT_MAX (0xFFFFFFFF).
    if ( is_unsigned && cmp_type && cmp_type->size == 4 )
    {
	if ( lval.isReg() && lval.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    x86::Gp l32 = pgm.cc.newGpd("cmp32_l");
	    pgm.cc.mov(l32, lval.as<x86::Gp>().r32());
	    left_norm = l32;
	    lval = left_norm;
	}
	if ( rval.isReg() && rval.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    x86::Gp r32 = pgm.cc.newGpd("cmp32_r");
	    pgm.cc.mov(r32, rval.as<x86::Gp>().r32());
	    right_norm = r32;
	    rval = right_norm;
	}
	// Imm truncation: mask to 32 bits
	if ( rval.isImm() )
	{
	    int64_t v = rval.as<Imm>().value();
	    right_norm = imm((int64_t)(uint32_t)v);
	    rval = right_norm;
	}
    }
    x86::Gp result = pgm.cc.newGpq("_cmp");
    pgm.safecmp(lval, rval, cmp_type);
    if ( cmp_type && cmp_type->is_real() )
    {
	x86::Gp aux = pgm.cc.newGpq("_cmp_aux");
	switch(op)
	{
	    case CmpKind::Eq:
		pgm.safesete(result);
		pgm.safesetnp(aux);
		pgm.safeand(result, aux, &ddINT64);
		break;
	    case CmpKind::Ne:
		pgm.safesetne(result);
		pgm.safesetp(aux);
		pgm.safeor(result, aux, &ddINT64);
		break;
	    case CmpKind::Lt:
		pgm.safesetb(result);
		pgm.safesetnp(aux);
		pgm.safeand(result, aux, &ddINT64);
		break;
	    case CmpKind::Le:
		pgm.safesetbe(result);
		pgm.safesetnp(aux);
		pgm.safeand(result, aux, &ddINT64);
		break;
	    case CmpKind::Gt:
		pgm.safeseta(result);
		break;
	    case CmpKind::Ge:
		pgm.safesetae(result);
		break;
	}
    }
    else
    {
	switch(op)
	{
	    case CmpKind::Eq: pgm.safesete(result);                                              break;
	    case CmpKind::Ne: pgm.safesetne(result);                                             break;
	    case CmpKind::Lt: if ( is_unsigned ) pgm.safesetb(result);  else pgm.safesetl(result);  break;
	    case CmpKind::Le: if ( is_unsigned ) pgm.safesetbe(result); else pgm.safesetle(result); break;
	    case CmpKind::Gt: if ( is_unsigned ) pgm.safeseta(result);  else pgm.safesetg(result);  break;
	    case CmpKind::Ge: if ( is_unsigned ) pgm.safesetae(result); else pgm.safesetge(result); break;
	}
    }
    regdp.second = &ddINT64;
    storage = result;
    regdp.first = &storage;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(result, &ddINT64), regdp, storage, &ddINT64);
    }
    return storage;
}

static Operand &emit_complex_value_from_expr(Program &pgm, TokenBase *expr,
					     DataDef *target_complex_type,
					     regdefp_t &regdp, Operand &storage)
{
    DataDefCOMPLEX *target_cdd = dynamic_cast<DataDefCOMPLEX *>(target_complex_type);
    if ( !target_cdd || !target_cdd->element_type )
	throw "emit_complex_value_from_expr() invalid target complex type";

    Operand base_storage;
    Operand real_storage;
    Operand imag_storage;
    Operand *real_part = NULL;
    Operand *imag_part = NULL;
    DataDef *real_type = NULL;
    DataDef *imag_type = NULL;
    prepare_complex_component_pair(pgm, expr, target_complex_type, base_storage,
				   real_part, real_type, imag_part, imag_type,
				   real_storage, imag_storage);

    Operand *caller_dest = regdp.first;
    bool use_caller_dest = caller_dest && (!regdp.second || regdp.second->is_complex());
    Operand dest_base;
    if ( use_caller_dest )
	dest_base = *caller_dest;
    else
	dest_base = pgm.cc.newStack((uint32_t)target_complex_type->size, 8);

    x86::Mem dst_real = complex_component_mem(pgm, dest_base, target_cdd, false, "__complex_cast_dst");
    x86::Mem dst_imag = complex_component_mem(pgm, dest_base, target_cdd, true, "__complex_cast_dst");

    IRBuilder ir(pgm.cc);
    IRValue real_val = ir.load(ir.coerce(ir_from_operand(*real_part, real_type),
					 target_cdd->element_type));
    IRValue imag_val = ir.load(ir.coerce(ir_from_operand(*imag_part, imag_type),
					 target_cdd->element_type));
    ir.store(IRValue::mem(dst_real, target_cdd->element_type), real_val);
    ir.store(IRValue::mem(dst_imag, target_cdd->element_type), imag_val);

    regdp.second = target_complex_type;
    if ( use_caller_dest )
	return *regdp.first;

    storage = dest_base;
    regdp.first = &storage;
    return storage;
}

static Operand &emit_union_from_scalar(Program &pgm, TokenBase *expr,
				       DataDefSTRUCT *union_type,
				       regdefp_t &regdp, Operand &storage)
{
    if ( !expr || !union_type || !union_type->union_layout || union_type->members.empty() )
	throw "emit_union_from_scalar() invalid union cast";

    DataDef *member_type = union_type->members[0].second;
    if ( !member_type || (!member_type->is_numeric() && !member_type->is_pointer()) )
	throw "emit_union_from_scalar() unsupported first union member type";

    Operand value_storage;
    Operand &value = compile_token_normalized(pgm, expr, member_type, nullptr, value_storage);

    size_t align = union_type->alignment();
    if ( align == 0 )
	align = 8;
    x86::Mem slot = pgm.cc.newStack((uint32_t)union_type->size, (uint32_t)align);
    x86::Mem member_mem = slot;
    member_mem.setSize((uint32_t)member_type->size);

    IRBuilder ir(pgm.cc);
    IRValue coerced = ir.load(ir.coerce(ir_from_operand(value, member_type), member_type));
    ir.store(IRValue::mem(member_mem, member_type), coerced);

    storage = as_gp_ptr(pgm, slot, "__union_cast");
    regdp.first = &storage;
    regdp.second = union_type;
    return storage;
}

// builtin_complex_compat_type and builtin_complex_arg_type moved to compiler_builtins.cpp.

static Operand &emit_complex_addsub(Program &pgm, TokenBase *left, TokenBase *right,
				    bool subtract, regdefp_t &regdp, Operand &storage)
{
    DataDef *left_type = left ? left->datadef() : NULL;
    DataDef *right_type = right ? right->datadef() : NULL;
    DataDef *complex_type = left_type && left_type->is_complex() ? left_type : right_type;
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_type);
    if ( !cdd || !cdd->element_type )
	throw "emit_complex_addsub() invalid complex type";

    Operand left_base_storage, right_base_storage;
    Operand left_real_storage, left_imag_storage, right_real_storage, right_imag_storage;
    Operand *left_real = NULL, *left_imag = NULL, *right_real = NULL, *right_imag = NULL;
    DataDef *left_real_type = NULL, *left_imag_type = NULL, *right_real_type = NULL, *right_imag_type = NULL;

    prepare_complex_component_pair(pgm, left, complex_type, left_base_storage,
				   left_real, left_real_type, left_imag, left_imag_type,
				   left_real_storage, left_imag_storage);
    prepare_complex_component_pair(pgm, right, complex_type, right_base_storage,
				   right_real, right_real_type, right_imag, right_imag_type,
				   right_real_storage, right_imag_storage);

    IRBuilder ir(pgm.cc);
    IRValue lhs_real_val = ir.load(ir.coerce(ir_from_operand(*left_real, left_real_type), cdd->element_type));
    IRValue lhs_imag_val = ir.load(ir.coerce(ir_from_operand(*left_imag, left_imag_type), cdd->element_type));
    IRValue rhs_real_val = ir.load(ir.coerce(ir_from_operand(*right_real, right_real_type), cdd->element_type));
    IRValue rhs_imag_val = ir.load(ir.coerce(ir_from_operand(*right_imag, right_imag_type), cdd->element_type));

    Operand real_out = lhs_real_val.op;
    Operand imag_out = lhs_imag_val.op;
    if ( subtract )
    {
	pgm.safesub(real_out, rhs_real_val.op, cdd->element_type);
	pgm.safesub(imag_out, rhs_imag_val.op, cdd->element_type);
    }
    else
    {
	pgm.safeadd(real_out, rhs_real_val.op, cdd->element_type);
	pgm.safeadd(imag_out, rhs_imag_val.op, cdd->element_type);
    }

    x86::Mem slot = pgm.cc.newStack((uint32_t)complex_type->size, 8);
    x86::Mem real_mem = slot;
    real_mem.setSize((uint32_t)cdd->element_type->size);
    x86::Mem imag_mem = slot;
    imag_mem.addOffset((int64_t)cdd->component_offset(true));
    imag_mem.setSize((uint32_t)cdd->element_type->size);
    ir.store(IRValue::mem(real_mem, cdd->element_type), ir_from_operand(real_out, cdd->element_type));
    ir.store(IRValue::mem(imag_mem, cdd->element_type), ir_from_operand(imag_out, cdd->element_type));

    storage = as_gp_ptr(pgm, slot, subtract ? "__complex_sub" : "__complex_add");
    regdp.first = &storage;
    regdp.second = complex_type;
    return storage;
}

static Operand &copy_typed_value(Program &pgm, const Operand &src, DataDef *type,
				 Operand &storage, const char *name)
{
    storage = type->newreg(pgm.cc, name);
    if ( storage.isReg() && storage.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( src.isReg() && src.as<BaseReg>().isGroup(RegGroup::kVec) )
	    pgm.safemov(storage.as<x86::Xmm>(), ((Operand &)src).as<x86::Xmm>(), type, type);
	else if ( src.isMem() )
	    pgm.safemov(storage.as<x86::Xmm>(), ((Operand &)src).as<x86::Mem>(), type, type);
	else if ( src.isReg() && src.as<BaseReg>().isGroup(RegGroup::kGp) )
	    pgm.safemov(storage.as<x86::Xmm>(), ((Operand &)src).as<x86::Gp>(), type, type);
	else
	    throw "copy_typed_value() unsupported Xmm source";
    }
    else if ( storage.isReg() && storage.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( src.isReg() && src.as<BaseReg>().isGroup(RegGroup::kGp) )
	    pgm.safemov(storage.as<x86::Gp>(), ((Operand &)src).as<x86::Gp>(), type, type);
	else if ( src.isMem() )
	    pgm.safemov(storage.as<x86::Gp>(), ((Operand &)src).as<x86::Mem>(), type, type);
	else if ( src.isImm() )
	    pgm.cc.mov(storage.as<x86::Gp>(), ((Operand &)src).as<Imm>());
	else if ( src.isReg() && src.as<BaseReg>().isGroup(RegGroup::kVec) )
	    pgm.safemov(storage.as<x86::Gp>(), ((Operand &)src).as<x86::Xmm>(), type, type);
	else
	    throw "copy_typed_value() unsupported Gp source";
    }
    else
	throw "copy_typed_value() unsupported destination";
    return storage;
}

static Operand &emit_complex_muldiv(Program &pgm, TokenBase *left, TokenBase *right,
				    bool divide, regdefp_t &regdp, Operand &storage)
{
    DataDef *left_type = left ? left->datadef() : NULL;
    DataDef *right_type = right ? right->datadef() : NULL;
    DataDef *complex_type = left_type && left_type->is_complex() ? left_type : right_type;
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_type);
    if ( !cdd || !cdd->element_type )
	throw "emit_complex_muldiv() invalid complex type";

    Operand left_base_storage, right_base_storage;
    Operand left_real_storage, left_imag_storage, right_real_storage, right_imag_storage;
    Operand *left_real = NULL, *left_imag = NULL, *right_real = NULL, *right_imag = NULL;
    DataDef *left_real_type = NULL, *left_imag_type = NULL, *right_real_type = NULL, *right_imag_type = NULL;

    prepare_complex_component_pair(pgm, left, complex_type, left_base_storage,
				   left_real, left_real_type, left_imag, left_imag_type,
				   left_real_storage, left_imag_storage);
    prepare_complex_component_pair(pgm, right, complex_type, right_base_storage,
				   right_real, right_real_type, right_imag, right_imag_type,
				   right_real_storage, right_imag_storage);

    IRBuilder ir(pgm.cc);
    IRValue lre = ir.load(ir.coerce(ir_from_operand(*left_real, left_real_type), cdd->element_type));
    IRValue lim = ir.load(ir.coerce(ir_from_operand(*left_imag, left_imag_type), cdd->element_type));
    IRValue rre = ir.load(ir.coerce(ir_from_operand(*right_real, right_real_type), cdd->element_type));
    IRValue rim = ir.load(ir.coerce(ir_from_operand(*right_imag, right_imag_type), cdd->element_type));

    Operand a, b, c, d;
    copy_typed_value(pgm, lre.op, cdd->element_type, a, "__complex_a");
    copy_typed_value(pgm, lim.op, cdd->element_type, b, "__complex_b");
    copy_typed_value(pgm, rre.op, cdd->element_type, c, "__complex_c");
    copy_typed_value(pgm, rim.op, cdd->element_type, d, "__complex_d");

    Operand ac, bd, ad, bc;
    copy_typed_value(pgm, a, cdd->element_type, ac, "__complex_ac");
    pgm.safemul(ac, c, cdd->element_type);
    copy_typed_value(pgm, b, cdd->element_type, bd, "__complex_bd");
    pgm.safemul(bd, d, cdd->element_type);
    copy_typed_value(pgm, a, cdd->element_type, ad, "__complex_ad");
    pgm.safemul(ad, d, cdd->element_type);
    copy_typed_value(pgm, b, cdd->element_type, bc, "__complex_bc");
    pgm.safemul(bc, c, cdd->element_type);

    Operand real_out, imag_out;
    if ( !divide )
    {
	copy_typed_value(pgm, ac, cdd->element_type, real_out, "__complex_mul_re");
	pgm.safesub(real_out, bd, cdd->element_type);
	copy_typed_value(pgm, ad, cdd->element_type, imag_out, "__complex_mul_im");
	pgm.safeadd(imag_out, bc, cdd->element_type);
    }
    else
    {
	if ( cdd->element_type->is_real() )
	{
	    x86::Mem slot = pgm.cc.newStack((uint32_t)complex_type->size, 8);
	    x86::Gp out_ptr = as_gp_ptr(pgm, slot, "__complex_div_out");
	    void *helper = cdd->element_type->size == sizeof(float)
		? (void *)madc_runtime_complex_div_float
		: (void *)madc_runtime_complex_div_double;

	    InvokeNode *call;
	    if ( cdd->element_type->size == sizeof(float) )
		pgm.cc.invoke(&call, imm(helper),
			      FuncSignature::build<void, void *, float, float, float, float>());
	    else
		pgm.cc.invoke(&call, imm(helper),
			      FuncSignature::build<void, void *, double, double, double, double>());
	    pgm.track_invoke_target(helper);
	    call->setArg(0, out_ptr);
	    call->setArg(1, a.as<x86::Xmm>());
	    call->setArg(2, b.as<x86::Xmm>());
	    call->setArg(3, c.as<x86::Xmm>());
	    call->setArg(4, d.as<x86::Xmm>());

	    storage = as_gp_ptr(pgm, slot, "__complex_div");
	    regdp.first = &storage;
	    regdp.second = complex_type;
	    return storage;
	}

	Operand denom_cc, denom_dd, denom, num_re, num_im;
	copy_typed_value(pgm, c, cdd->element_type, denom_cc, "__complex_cc");
	pgm.safemul(denom_cc, c, cdd->element_type);
	copy_typed_value(pgm, d, cdd->element_type, denom_dd, "__complex_dd");
	pgm.safemul(denom_dd, d, cdd->element_type);
	copy_typed_value(pgm, denom_cc, cdd->element_type, denom, "__complex_den");
	pgm.safeadd(denom, denom_dd, cdd->element_type);

	copy_typed_value(pgm, ac, cdd->element_type, num_re, "__complex_num_re");
	pgm.safeadd(num_re, bd, cdd->element_type);
	copy_typed_value(pgm, bc, cdd->element_type, num_im, "__complex_num_im");
	pgm.safesub(num_im, ad, cdd->element_type);

	Operand rem_re, rem_im;
	rem_re = cdd->element_type->newreg(pgm.cc, "__complex_rem_re");
	rem_im = cdd->element_type->newreg(pgm.cc, "__complex_rem_im");
	if ( cdd->element_type->is_integer() )
	{
	    pgm.safexor(rem_re, rem_re);
	    pgm.safexor(rem_im, rem_im);
	}
	pgm.safediv(rem_re, num_re, denom, cdd->element_type);
	pgm.safediv(rem_im, num_im, denom, cdd->element_type);
	real_out = num_re;
	imag_out = num_im;
    }

    x86::Mem slot = pgm.cc.newStack((uint32_t)complex_type->size, 8);
    x86::Mem real_mem = slot;
    real_mem.setSize((uint32_t)cdd->element_type->size);
    x86::Mem imag_mem = slot;
    imag_mem.addOffset((int64_t)cdd->component_offset(true));
    imag_mem.setSize((uint32_t)cdd->element_type->size);
    ir.store(IRValue::mem(real_mem, cdd->element_type), ir_from_operand(real_out, cdd->element_type));
    ir.store(IRValue::mem(imag_mem, cdd->element_type), ir_from_operand(imag_out, cdd->element_type));

    storage = as_gp_ptr(pgm, slot, divide ? "__complex_div" : "__complex_mul");
    regdp.first = &storage;
    regdp.second = complex_type;
    return storage;
}

static Operand &emit_complex_compound_muldiv(Program &pgm, TokenBase *left, TokenBase *right,
					     bool divide, regdefp_t &regdp, Operand &storage)
{
    DataDef *complex_type = left ? left->datadef() : NULL;
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_type);
    if ( !cdd || !cdd->element_type )
	throw "emit_complex_compound_muldiv() invalid complex lhs";

    regdefp_t lhs_rdp = {NULL, complex_type, NULL};
    Operand &lhs_base = left->compile(pgm, lhs_rdp);
    x86::Mem lhs_real = complex_component_mem(pgm, lhs_base, cdd, false, "__complex_lhs");
    x86::Mem lhs_imag = complex_component_mem(pgm, lhs_base, cdd, true, "__complex_lhs");

    Operand right_base_storage, right_real_storage, right_imag_storage;
    Operand *right_real = NULL, *right_imag = NULL;
    DataDef *right_real_type = NULL, *right_imag_type = NULL;
    prepare_complex_component_pair(pgm, right, complex_type, right_base_storage,
				   right_real, right_real_type, right_imag, right_imag_type,
				   right_real_storage, right_imag_storage);

    IRBuilder ir(pgm.cc);
    IRValue lre = ir.load(IRValue::mem(lhs_real, cdd->element_type));
    IRValue lim = ir.load(IRValue::mem(lhs_imag, cdd->element_type));
    IRValue rre = ir.load(ir.coerce(ir_from_operand(*right_real, right_real_type), cdd->element_type));
    IRValue rim = ir.load(ir.coerce(ir_from_operand(*right_imag, right_imag_type), cdd->element_type));

    Operand a, b, c, d;
    copy_typed_value(pgm, lre.op, cdd->element_type, a, "__complex_ca");
    copy_typed_value(pgm, lim.op, cdd->element_type, b, "__complex_cb");
    copy_typed_value(pgm, rre.op, cdd->element_type, c, "__complex_cc");
    copy_typed_value(pgm, rim.op, cdd->element_type, d, "__complex_cd");

    Operand ac, bd, ad, bc;
    copy_typed_value(pgm, a, cdd->element_type, ac, "__complex_cac");
    pgm.safemul(ac, c, cdd->element_type);
    copy_typed_value(pgm, b, cdd->element_type, bd, "__complex_cbd");
    pgm.safemul(bd, d, cdd->element_type);
    copy_typed_value(pgm, a, cdd->element_type, ad, "__complex_cad");
    pgm.safemul(ad, d, cdd->element_type);
    copy_typed_value(pgm, b, cdd->element_type, bc, "__complex_cbc");
    pgm.safemul(bc, c, cdd->element_type);

    Operand real_out, imag_out;
    if ( !divide )
    {
	copy_typed_value(pgm, ac, cdd->element_type, real_out, "__complex_cmre");
	pgm.safesub(real_out, bd, cdd->element_type);
	copy_typed_value(pgm, ad, cdd->element_type, imag_out, "__complex_cmim");
	pgm.safeadd(imag_out, bc, cdd->element_type);
    }
    else
    {
	Operand denom_cc, denom_dd, denom, num_re, num_im;
	copy_typed_value(pgm, c, cdd->element_type, denom_cc, "__complex_cdcc");
	pgm.safemul(denom_cc, c, cdd->element_type);
	copy_typed_value(pgm, d, cdd->element_type, denom_dd, "__complex_cddd");
	pgm.safemul(denom_dd, d, cdd->element_type);
	copy_typed_value(pgm, denom_cc, cdd->element_type, denom, "__complex_cden");
	pgm.safeadd(denom, denom_dd, cdd->element_type);

	copy_typed_value(pgm, ac, cdd->element_type, num_re, "__complex_cnre");
	pgm.safeadd(num_re, bd, cdd->element_type);
	copy_typed_value(pgm, bc, cdd->element_type, num_im, "__complex_cnim");
	pgm.safesub(num_im, ad, cdd->element_type);

	Operand rem_re, rem_im;
	rem_re = cdd->element_type->newreg(pgm.cc, "__complex_crem_re");
	rem_im = cdd->element_type->newreg(pgm.cc, "__complex_crem_im");
	if ( cdd->element_type->is_integer() )
	{
	    pgm.safexor(rem_re, rem_re);
	    pgm.safexor(rem_im, rem_im);
	}
	pgm.safediv(rem_re, num_re, denom, cdd->element_type);
	pgm.safediv(rem_im, num_im, denom, cdd->element_type);
	real_out = num_re;
	imag_out = num_im;
    }

    ir.store(IRValue::mem(lhs_real, cdd->element_type), ir_from_operand(real_out, cdd->element_type));
    ir.store(IRValue::mem(lhs_imag, cdd->element_type), ir_from_operand(imag_out, cdd->element_type));
    storage = lhs_base;
    regdp.first = &storage;
    regdp.second = complex_type;
    return storage;
}

static Operand &emit_complex_compound_addsub(Program &pgm, TokenBase *left, TokenBase *right,
					     bool subtract, regdefp_t &regdp, Operand &storage)
{
    DataDef *complex_type = left ? left->datadef() : NULL;
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_type);
    if ( !cdd || !cdd->element_type )
	throw "emit_complex_compound_addsub() invalid complex lhs";

    regdefp_t lhs_rdp = {NULL, complex_type, NULL};
    Operand &lhs_base = left->compile(pgm, lhs_rdp);
    x86::Mem lhs_real = complex_component_mem(pgm, lhs_base, cdd, false, "__complex_lhs");
    x86::Mem lhs_imag = complex_component_mem(pgm, lhs_base, cdd, true, "__complex_lhs");

    Operand rhs_base_storage, rhs_real_storage, rhs_imag_storage;
    Operand *rhs_real = NULL, *rhs_imag = NULL;
    DataDef *rhs_real_type = NULL, *rhs_imag_type = NULL;
    prepare_complex_component_pair(pgm, right, complex_type, rhs_base_storage,
				   rhs_real, rhs_real_type, rhs_imag, rhs_imag_type,
				   rhs_real_storage, rhs_imag_storage);

    IRBuilder ir(pgm.cc);
    IRValue lhs_real_val = ir.load(IRValue::mem(lhs_real, cdd->element_type));
    IRValue lhs_imag_val = ir.load(IRValue::mem(lhs_imag, cdd->element_type));
    IRValue rhs_real_val = ir.load(ir.coerce(ir_from_operand(*rhs_real, rhs_real_type), cdd->element_type));
    IRValue rhs_imag_val = ir.load(ir.coerce(ir_from_operand(*rhs_imag, rhs_imag_type), cdd->element_type));

    Operand real_out = lhs_real_val.op;
    Operand imag_out = lhs_imag_val.op;
    if ( subtract )
    {
	pgm.safesub(real_out, rhs_real_val.op, cdd->element_type);
	pgm.safesub(imag_out, rhs_imag_val.op, cdd->element_type);
    }
    else
    {
	pgm.safeadd(real_out, rhs_real_val.op, cdd->element_type);
	pgm.safeadd(imag_out, rhs_imag_val.op, cdd->element_type);
    }

    ir.store(IRValue::mem(lhs_real, cdd->element_type), ir_from_operand(real_out, cdd->element_type));
    ir.store(IRValue::mem(lhs_imag, cdd->element_type), ir_from_operand(imag_out, cdd->element_type));

    storage = lhs_base;
    regdp.first = &storage;
    regdp.second = complex_type;
    return storage;
}

Operand &emit_complex_from_scalar(Program &pgm, Operand &scalar_op, DataDef *scalar_type,
					 DataDef *complex_type, bool imag_value,
					 regdefp_t &regdp, Operand &storage,
					 const char *slot_name)
{
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_type);
    if ( !cdd || !cdd->element_type )
	throw "emit_complex_from_scalar() invalid complex type";

    x86::Mem slot = pgm.cc.newStack((uint32_t)complex_type->size, 8);
    x86::Mem real_mem = slot;
    real_mem.setSize((uint32_t)cdd->element_type->size);
    x86::Mem imag_mem = slot;
    imag_mem.addOffset((int64_t)cdd->component_offset(true));
    imag_mem.setSize((uint32_t)cdd->element_type->size);

    IRBuilder ir(pgm.cc);
    IRValue scalar_val = ir.load(ir.coerce(ir_from_operand(scalar_op, scalar_type), cdd->element_type));
    if ( imag_value )
    {
	ir.store(IRValue::mem(real_mem, cdd->element_type), IRValue::imm(Imm(0), &ddINT));
	ir.store(IRValue::mem(imag_mem, cdd->element_type), scalar_val);
    }
    else
    {
	ir.store(IRValue::mem(real_mem, cdd->element_type), scalar_val);
	ir.store(IRValue::mem(imag_mem, cdd->element_type), IRValue::imm(Imm(0), &ddINT));
    }

    storage = as_gp_ptr(pgm, slot, slot_name);
    regdp.first = &storage;
    regdp.second = complex_type;
    return storage;
}





static void emit_bitfield_byteswap(Program &pgm, x86::Gp &gp, size_t storage_size)
{
    if ( storage_size <= 1 )
	return;
    if ( storage_size == 2 )
    {
	pgm.cc.rol(gp.r16(), imm(8));
	return;
    }
    if ( storage_size == 4 )
    {
	pgm.cc.bswap(gp.r32());
	return;
    }
    pgm.cc.bswap(gp.r64());
}

static void emit_and_u64(Program &pgm, x86::Gp &gp, uint64_t mask, const char *hint);




x86::Gp emit_bitfield_load_storage(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, const char *hint)
{
    storage.setSize((uint32_t)bf.storage_size);
    x86::Gp out = pgm.cc.newGpq("%s_storage", hint ? hint : "bf");
    if ( bf.storage_size == 8 )
	pgm.cc.mov(out, storage);
    else if ( bf.storage_size == 4 )
	pgm.cc.mov(out.r32(), storage);
    else if ( bf.storage_size == 2 )
	pgm.cc.movzx(out, storage);
    else
	pgm.cc.movzx(out, storage);
    if ( bf.reverse_storage )
	emit_bitfield_byteswap(pgm, out, bf.storage_size);
    return out;
}

void emit_bitfield_store_storage(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, x86::Gp value)
{
    x86::Gp stored = value;
    if ( bf.reverse_storage )
    {
	stored = pgm.cc.newGpq("bf_store_swapped");
	pgm.cc.mov(stored, value);
	emit_bitfield_byteswap(pgm, stored, bf.storage_size);
    }
    storage.setSize((uint32_t)bf.storage_size);
    if ( bf.storage_size == 8 )
	pgm.cc.mov(storage, stored.r64());
    else if ( bf.storage_size == 4 )
	pgm.cc.mov(storage, stored.r32());
    else if ( bf.storage_size == 2 )
	pgm.cc.mov(storage, stored.r16());
    else
	pgm.cc.mov(storage, stored.r8());
}

void emit_bitfield_sign_extend(Program &pgm, x86::Gp &value,
    const DataDefSTRUCT::BitFieldInfo &bf)
{
    if ( bf.is_unsigned || bf.bit_width == 0 || bf.bit_width >= 64 )
	return;
    uint32_t shift = (uint32_t)(64 - bf.bit_width);
    pgm.cc.shl(value, imm(shift));
    pgm.cc.sar(value, imm(shift));
}

static Operand &finish_compound_assign(Program &pgm, regdefp_t &regdp,
    CompoundLHS &lhs, Operand &_operand, Operand *out)
{
    lhs.finish(pgm);
    regdp.second = lhs.type;
    if ( out )
    {
	pgm.safemov(*out, lhs.lval, lhs.type, lhs.type);
	regdp.first = out;
	return *out;
    }
    _operand = lhs.lval;
    regdp.first = &_operand;
    return _operand;
}

static bool large_simd_lvalue_mem(Program &pgm, TokenBase *left, DataDefSIMD *vdd,
				  x86::Mem &out)
{
    if ( !left || !vdd )
	return false;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(left) )
    {
	Operand &op = tv->operand(pgm);
	if ( op.isMem() )
	{
	    out = sized_simd_mem(op.as<x86::Mem>(), vdd);
	    return true;
	}
	if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    out = x86::ptr(op.as<x86::Gp>(), 0, (uint32_t)vdd->size);
	    return true;
	}
	return false;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(left) )
    {
	Operand &op = tm->operand(pgm);
	if ( op.isMem() )
	{
	    out = sized_simd_mem(op.as<x86::Mem>(), vdd);
	    return true;
	}
	return false;
    }
    if ( TokenDeref *td = dynamic_cast<TokenDeref *>(left) )
    {
	Operand &op = td->operand(pgm);
	if ( op.isMem() )
	{
	    out = sized_simd_mem(op.as<x86::Mem>(), vdd);
	    return true;
	}
	return false;
    }
    if ( TokenDerefExpr *tde = dynamic_cast<TokenDerefExpr *>(left) )
    {
	Operand &op = tde->operand(pgm);
	if ( op.isMem() )
	{
	    out = sized_simd_mem(op.as<x86::Mem>(), vdd);
	    return true;
	}
	return false;
    }
    return false;
}

static bool try_emit_large_simd_compound_bitop(Program &pgm, TokenBase *left, TokenBase *right,
					       SafeBitOp2 op, regdefp_t &regdp,
					       Operand &_operand)
{
    DataDefSIMD *vdd = left && left->datadef() && is_large_simd_type(left->datadef())
	? static_cast<DataDefSIMD *>(left->datadef()) : NULL;
    if ( !vdd || !vdd->element_type || !vdd->element_type->is_integer() )
	return false;

    x86::Mem dst;
    if ( !large_simd_lvalue_mem(pgm, left, vdd, dst) )
	return false;

    DataDef *elem = vdd->element_type;
    bool right_vec = expr_contains_simd_value(right);
    x86::Mem right_mem;
    Operand right_scalar;
    if ( right_vec )
	right_mem = large_simd_expr_mem(pgm, right, vdd, "simd_cmpd_r");
    else
	right_scalar = compile_token_normalized(pgm, right, elem, nullptr, right_scalar);

    for ( size_t i = 0; i < vdd->lane_count; ++i )
    {
	Operand ltmp = load_simd_lane_gp(pgm, dst, i, vdd, "simd_cmpd_l");
	Operand rtmp;
	Operand &rval = right_vec
	    ? (rtmp = load_simd_lane_gp(pgm, right_mem, i, vdd, "simd_cmpd_r"))
	    : right_scalar;
	(pgm.*op)(ltmp, rval, elem);
	x86::Mem slot = simd_lane_mem(dst, i, vdd);
	pgm.safemov(slot, ltmp, elem, elem);
    }

    if ( TokenVar *tv = dynamic_cast<TokenVar *>(left) )
	tv->var.modified();
    regdp.second = vdd;
    _operand = dst;
    regdp.first = &_operand;
    return true;
}

// Determine the DataDef type of a compound-assignment LHS without reading
// the value. Used to normalize the RHS before the LHS value is loaded.
static DataDef *compound_lhs_type(Program &pgm, TokenBase *left)
{
    if ( left->type() == TokenType::ttVariable )
    {
	TokenVar *tv = dynamic_cast<TokenVar *>(left);
	return tv ? tv->var.type : left->datadef();
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(left) )
	return tm->datadef();
    if ( TokenSubscript *ts = dynamic_cast<TokenSubscript *>(left) )
	return ts->datadef();
    if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(left) )
	return tse->datadef();
    return left->datadef();
}

static CompoundLHS resolveCompoundLHS(Program &pgm, TokenBase *left, const char *op_name)
{
    CompoundLHS r;
    r.tv = NULL;
    r.is_member = false;
    r.is_bitfield = false;
    r.type = NULL;

    if ( left->type() == TokenType::ttVariable )
    {
	r.tv = dynamic_cast<TokenVar *>(left);
	r.type = r.tv->var.type;
	Operand &op = r.tv->operand(pgm);
	if ( op.isMem() )
	{
	    if ( r.type && (r.type->is_real() || r.type->is_simd()) )
	    {
		x86::Xmm xmm = pgm.cc.newXmm("var_lhs_xmm");
		x86::Mem mem = op.as<x86::Mem>();
		load_mem_to_xmm(pgm, xmm, mem, r.type);
		r.writeback = mem;
		r.lval = xmm;
		r.is_member = true;
	    }
	    else
	    {
		x86::Gp gp = pgm.cc.newGpq("var_lhs");
		load_mem_to_gpq(pgm, gp, op.as<x86::Mem>(), r.type);
		r.writeback = op.as<x86::Mem>();
		r.lval = gp;
		r.is_member = true;
		// lval is widened to Gpq; work in ddINT64 so tmp / rval / safeop
		// match its width. Writeback Mem keeps its original size so the
		// final store truncates correctly.
		if ( r.type && r.type->size && r.type->size < 8 )
		    r.type = &ddINT64;
	    }
	}
	else
	    r.lval = op;
    }
    else if ( TokenDerefStep *tds = dynamic_cast<TokenDerefStep *>(left) )
    {
	r.type = tds->deref_type;
	Operand &ptr_op = pgm.tkFunction->voperand(pgm, &tds->var);
	if ( !ptr_op.isReg() || !ptr_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	    throw "resolveCompoundLHS(TokenDerefStep): pointer operand is not a gp register";

	x86::Gp ptr_reg = ptr_op.as<x86::Gp>();
	x86::Gp old_ptr = pgm.cc.newIntPtr(tds->increment ? "cmpd_deref_postinc_ptr" : "cmpd_deref_postdec_ptr");
	pgm.safemov(old_ptr, ptr_reg);

	int step = r.type ? (int)r.type->size : 1;
	if ( step <= 0 )
	    step = 1;
	if ( tds->increment )
	    pgm.safeadd(ptr_reg, step, tds->var.type, r.type);
	else
	    pgm.safesub(ptr_reg, step, tds->var.type, r.type);
	tds->var.modified();

	x86::Mem mem = x86::ptr(old_ptr, 0, (uint32_t)(r.type && r.type->size ? r.type->size : 8));
	if ( r.type && (r.type->is_real() || r.type->is_simd()) )
	{
	    r.writeback = mem;
	    x86::Xmm xmm = pgm.cc.newXmm("deref_step_lhs_xmm");
	    load_mem_to_xmm(pgm, xmm, mem, r.type);
	    r.lval = xmm;
	    r.is_member = true;
	}
	else
	{
	    x86::Gp gp = pgm.cc.newGpq("deref_step_lhs");
	    load_mem_to_gpq(pgm, gp, mem, r.type);
	    r.writeback = mem;
	    r.lval = gp;
	    r.is_member = true;
	    if ( r.type && r.type->size && r.type->size < 8 )
		r.type = &ddINT64;
	}
    }
    else if ( left->type() == TokenType::ttMember )
    {
	// struct member via . or -> : load Mem into temp register
	TokenMember *tm = dynamic_cast<TokenMember *>(left);
	if ( tm )
	{
	    r.type = tm->var.type;
	    Operand &mem = tm->operand(pgm);
	    if ( const DataDefSTRUCT::BitFieldInfo *bf = tm->bitfield_info() )
	    {
		if ( !mem.isMem() )
		    pgm.Throw(left) << "compound assignment " << op_name << " on bit-field with non-memory storage" << flush;
		x86::Mem storage = mem.as<x86::Mem>();
		storage.setSize((uint32_t)bf->storage_size);
		r.lval = emit_bitfield_load(pgm, storage, *bf, "member_bf_lhs");
		r.writeback = storage;
		r.is_member = true;
		r.is_bitfield = true;
		r.bitfield = *bf;
		return r;
	    }
	    if ( mem.isMem() )
	    {
		if ( r.type && (r.type->is_real() || r.type->is_simd()) )
		{
		    x86::Mem mm = mem.as<x86::Mem>();
		    x86::Xmm xmm = pgm.cc.newXmm("member_lhs_xmm");
		    load_mem_to_xmm(pgm, xmm, mm, r.type);
		    r.writeback = mm;
		    r.lval = xmm;
		    r.is_member = true;
		}
		else
		{
		    x86::Gp gp = pgm.cc.newGpq("member_lhs");
		    load_mem_to_gpq(pgm, gp, mem.as<x86::Mem>(), r.type);
		    r.writeback = mem.as<x86::Mem>();
		    r.lval = gp;
		    r.is_member = true;
		    if ( r.type && r.type->size && r.type->size < 8 )
			r.type = &ddINT64;
		}
	    }
	    else
		r.lval = mem;
	}
	else
	{
	    // TokenDeref also uses ttMember type
	    TokenDeref *td = dynamic_cast<TokenDeref *>(left);
	    TokenDerefExpr *tde = td ? NULL : dynamic_cast<TokenDerefExpr *>(left);
	    DataDef *deref_t = td ? td->deref_type : (tde ? tde->deref_type : NULL);
	    if ( td || tde )
	    {
		r.type = deref_t;
		Operand &mem = td ? td->operand(pgm) : tde->operand(pgm);
		if ( mem.isMem() )
		{
		    if ( r.type && (r.type->is_real() || r.type->is_simd()) )
		    {
			x86::Mem mm = mem.as<x86::Mem>();
			x86::Xmm xmm = pgm.cc.newXmm("deref_lhs_xmm");
			load_mem_to_xmm(pgm, xmm, mm, r.type);
			r.writeback = mm;
			r.lval = xmm;
			r.is_member = true;
		    }
		    else
		    {
			x86::Gp gp = pgm.cc.newGpq("deref_lhs");
			load_mem_to_gpq(pgm, gp, mem.as<x86::Mem>(), r.type);
			r.writeback = mem.as<x86::Mem>();
			r.lval = gp;
			r.is_member = true;
			if ( r.type && r.type->size && r.type->size < 8 )
			    r.type = &ddINT64;
		    }
		}
		else
		    r.lval = mem;
	    }
	    else if ( TokenDerefStep *tds = dynamic_cast<TokenDerefStep *>(left) )
	    {
		r.type = tds->deref_type;
		Operand &ptr_op = pgm.tkFunction->voperand(pgm, &tds->var);
		if ( !ptr_op.isReg() || !ptr_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		    throw "resolveCompoundLHS(TokenDerefStep): pointer operand is not a gp register";

		x86::Gp ptr_reg = ptr_op.as<x86::Gp>();
		x86::Gp old_ptr = pgm.cc.newIntPtr(tds->increment ? "cmpd_deref_postinc_ptr" : "cmpd_deref_postdec_ptr");
		pgm.safemov(old_ptr, ptr_reg);

		int step = r.type ? (int)r.type->size : 1;
		if ( step <= 0 )
		    step = 1;
		if ( tds->increment )
		    pgm.safeadd(ptr_reg, step, tds->var.type, r.type);
		else
		    pgm.safesub(ptr_reg, step, tds->var.type, r.type);
		tds->var.modified();

		x86::Mem mem = x86::ptr(old_ptr, 0, (uint32_t)(r.type && r.type->size ? r.type->size : 8));
		if ( r.type && (r.type->is_real() || r.type->is_simd()) )
		{
		    r.writeback = mem;
		    x86::Xmm xmm = pgm.cc.newXmm("deref_step_lhs_xmm");
		    load_mem_to_xmm(pgm, xmm, mem, r.type);
		    r.lval = xmm;
		    r.is_member = true;
		}
		else
		{
		    x86::Gp gp = pgm.cc.newGpq("deref_step_lhs");
		    load_mem_to_gpq(pgm, gp, mem, r.type);
		    r.writeback = mem;
		    r.lval = gp;
		    r.is_member = true;
		    if ( r.type && r.type->size && r.type->size < 8 )
			r.type = &ddINT64;
		}
	    }
	    else
		pgm.Throw(left) << "compound assignment " << op_name << " on unsupported member type" << flush;
	}
    }
    else if ( left->type() == TokenType::ttSubscript )
    {
	// Array / pointer subscript lvalue (`arr[i] += N`). Materialise the
	// element Mem directly so the load/compute/store cycle can write
	// back through the same address. TokenSubscript::compile reads into
	// a Gp when no destination is given, which would hide the Mem, so
	// we reconstruct the element address here.
	TokenSubscript *ts = dynamic_cast<TokenSubscript *>(left);
	if ( ts && ts->object.is_fixed_array() )
	{
	    size_t elem_size = ts->object.type->size ? ts->object.type->size : 8;
	    Operand &obj_op = pgm.tkFunction->voperand(pgm, &ts->object);
	    x86::Gp obj_reg = pgm.cc.newIntPtr("sub_obj");
	    pgm.cc.mov(obj_reg, obj_op.as<x86::Gp>());
	    regdefp_t idx_rdp = {NULL, NULL, NULL};
	    Operand &idx_op = ts->index->compile(pgm, idx_rdp);
	    x86::Gp idx_reg = pgm.cc.newGpq("sub_idx");
	    load_idx_to_gpq(pgm, idx_reg, idx_op);
	    uint32_t shift = 0;
	    if      ( elem_size == 8 ) shift = 3;
	    else if ( elem_size == 4 ) shift = 2;
	    else if ( elem_size == 2 ) shift = 1;
	    x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
	    r.type = ts->object.type;
	    x86::Gp gp = pgm.cc.newGpq("sub_lhs");
	    load_mem_to_gpq(pgm, gp, elem_mem, r.type);
	    r.writeback = elem_mem;
	    r.lval = gp;
	    r.is_member = true;
	    if ( r.type && r.type->size && r.type->size < 8 )
		r.type = &ddINT64;
	}
	else if ( ts && ts->object.type && ts->object.type->is_pointer() )
	{
	    // Raw-pointer subscript lvalue (`int *p; p[i] += N;`). Mirror
	    // TokenSubscript::compile()'s pointer-subscript read path: MOV
	    // the pointer value into a Gp, compute [ptr + idx*esize], load
	    // through that Mem, write-back through the same Mem.
	    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(ts->object.type);
	    DataDef *elem_type = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
	    size_t elem_size = elem_type->size ? elem_type->size : 8;
	    Operand &obj_op = pgm.tkFunction->voperand(pgm, &ts->object);
	    x86::Gp obj_reg = pgm.cc.newIntPtr("cmpd_subptr_obj");
	    pgm.cc.mov(obj_reg, obj_op.as<x86::Gp>());

	    regdefp_t idx_rdp = {NULL, NULL, NULL};
	    Operand &idx_op = ts->index->compile(pgm, idx_rdp);
	    x86::Gp idx_reg = pgm.cc.newGpq("cmpd_subptr_idx");
	    load_idx_to_gpq(pgm, idx_reg, idx_op);

	    uint32_t shift = 0;
	    if      ( elem_size == 8 ) shift = 3;
	    else if ( elem_size == 4 ) shift = 2;
	    else if ( elem_size == 2 ) shift = 1;
	    else if ( elem_size != 1 )
		pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)elem_size));
	    x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);

	    r.type = elem_type;
	    x86::Gp gp = pgm.cc.newGpq("cmpd_subptr_lhs");
	    load_mem_to_gpq(pgm, gp, elem_mem, r.type);
	    r.writeback = elem_mem;
	    r.lval = gp;
	    r.is_member = true;
	    if ( r.type && r.type->size && r.type->size < 8 )
		r.type = &ddINT64;
	}
	else if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(left) )
	{
	    // Expression-base subscript lvalue (`obj.bits[i] &= ~mask;`,
	    // `ptr->arr[j] += n;`). Mirror TokenAssign's TokenSubscriptExpr
	    // path: compile the base through operand() (avoid the
	    // emit_ir_value load-first-element trap), LEA if the base is an
	    // in-place aggregate / MOV if it's a pointer value, fold the
	    // element stride via imul for non-power-of-2 sizes.
	    DataDef *elem_type = tse->datadef();
	    if ( !elem_type )
		pgm.Throw(tse) << "compound-assign " << op_name << " on subscript expression with no element type" << flush;
	    size_t elem_size = elem_type->size ? elem_type->size : 8;

	    Operand &base_op = tse->base_expr->operand(pgm);
	    x86::Gp base_reg = pgm.cc.newIntPtr("cmpd_sub_base");
	    if ( base_op.isReg() && base_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.mov(base_reg, base_op.as<x86::Gp>());
	    else if ( base_op.isMem() )
	    {
		DataDef *bdd = tse->base_expr->datadef();
		if ( bdd && bdd->is_pointer() && !is_fixed_array_struct_member(tse->base_expr) )
		    pgm.cc.mov(base_reg, base_op.as<x86::Mem>());
		else
		    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
	    }
	    else
		pgm.Throw(tse) << "compound-assign " << op_name << " subscript base is not a register or memory" << flush;

	    regdefp_t idx_rdp = {NULL, NULL, NULL};
	    Operand &idx_op = tse->index->compile(pgm, idx_rdp);
	    x86::Gp idx_reg = pgm.cc.newGpq("cmpd_sub_idx");
	    load_idx_to_gpq(pgm, idx_reg, idx_op);

	    uint32_t shift = 0;
	    if      ( elem_size == 8 ) shift = 3;
	    else if ( elem_size == 4 ) shift = 2;
	    else if ( elem_size == 2 ) shift = 1;
	    else if ( elem_size != 1 )
		pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)elem_size));
	    x86::Mem elem_mem = x86::ptr(base_reg, idx_reg, shift, 0, (uint32_t)elem_size);

	    r.type = elem_type;
	    x86::Gp gp = pgm.cc.newGpq("cmpd_sub_lhs");
	    load_mem_to_gpq(pgm, gp, elem_mem, r.type);
	    r.writeback = elem_mem;
	    r.lval = gp;
	    r.is_member = true;
	    if ( r.type && r.type->size && r.type->size < 8 )
		r.type = &ddINT64;
	}
	else
	    pgm.Throw(left) << op_name << " on unsupported subscript lval" << flush;
    }
    else
	pgm.Throw(left) << op_name << " on a non-variable lval" << flush;
    return r;
}

static int pointer_step_size(DataDef *type)
{
    DataDefPTR *ptr = dynamic_cast<DataDefPTR *>(type);
    if ( !ptr || !ptr->base_type )
	return 1;
    int step = (int)ptr->base_type->size;
    return step > 0 ? step : 1;
}

static void apply_inc_dec_step(Program &pgm, Operand &op, DataDef *type,
			       bool increment, SafeUnaryStep fallback)
{
    if ( type && type->is_simd() )
    {
	x86::Xmm value;
	bool writeback_mem = op.isMem();
	if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kVec) )
	    value = op.as<x86::Xmm>();
	else if ( writeback_mem )
	{
	    value = pgm.cc.newXmm(increment ? "simd_inc" : "simd_dec");
	    x86::Mem mem = op.as<x86::Mem>();
	    load_mem_to_xmm(pgm, value, mem, type);
	}
	else
	    pgm.Throw << "SIMD ++/-- operand is not an Xmm register or memory" << flush;

	DataDefSIMD *vdd = static_cast<DataDefSIMD *>(type);
	x86::Gp one_i = pgm.cc.newGpq("simd_one_i");
	pgm.cc.mov(one_i, 1);
	x86::Xmm one = pgm.cc.newXmm("simd_one");
	splat_gp_to_simd_xmm(pgm, one, one_i, vdd);
	if ( increment )
	    pgm.safeadd(value, one, type, type);
	else
	    pgm.safesub(value, one, type, type);
	if ( writeback_mem )
	    store_xmm_to_mem(pgm, op.as<x86::Mem>(), value, type);
	return;
    }

    if ( type && type->is_real() )
    {
	x86::Xmm value;
	bool writeback_mem = op.isMem();
	if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kVec) )
	    value = op.as<x86::Xmm>();
	else if ( writeback_mem )
	{
	    value = newScalarXmm(pgm, type, increment ? "real_inc" : "real_dec");
	    pgm.safemov(value, op.as<x86::Mem>(), type, type);
	}
	else
	    pgm.Throw << "real ++/-- operand is not an Xmm register or memory" << flush;

	x86::Gp one_i = pgm.cc.newGpq("real_one_i");
	x86::Xmm one = newScalarXmm(pgm, type, "real_one");
	pgm.cc.mov(one_i, 1);
	pgm.safemov(one, one_i, type, &ddINT64);
	if ( type->size == sizeof(float) )
	{
	    if ( increment ) pgm.cc.addss(value, one);
	    else             pgm.cc.subss(value, one);
	}
	else
	{
	    if ( increment ) pgm.cc.addsd(value, one);
	    else             pgm.cc.subsd(value, one);
	}
	if ( writeback_mem )
	    pgm.safemov(op.as<x86::Mem>(), value, type, type);
	return;
    }

    int step = pointer_step_size(type);
    if ( type && type->is_pointer() && step != 1 )
    {
	if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    if ( increment ) pgm.cc.add(op.as<x86::Gp>(), imm(step));
	    else             pgm.cc.sub(op.as<x86::Gp>(), imm(step));
	    return;
	}
	if ( op.isMem() )
	{
	    if ( increment ) pgm.cc.add(op.as<x86::Mem>(), imm(step));
	    else             pgm.cc.sub(op.as<x86::Mem>(), imm(step));
	    return;
	}
    }
    (pgm.*fallback)(op);
}

// Emit ++target or --target, depending on `step`. Handles all four
// combinations of target shape (plain variable vs. member/deref lvalue)
// and position (postfix — return old value — vs. prefix — return new).
// The `old_name_hint` / `new_name_hint` parameters feed into the
// temp-register names so ++/-- keep readable asm in logs.
static Operand &emit_inc_dec(Program &pgm, TokenBase *caller, TokenBase *target,
			     bool postfix,
			     SafeUnaryStep step, regdefp_t &regdp, Operand &_operand,
			     const char *old_name_hint, const char *new_name_hint,
			     const char *op_name)
{
    TokenVar *tv = dynamic_cast<TokenVar *>(target);
    if ( tv && tv->var.is_constant() )
	pgm.Throw(caller) << op_name << " on const variable '" << tv->var.name << "'" << flush;
    bool increment = op_name && op_name[0] == '+';
    TokenDerefExpr *postfix_deref = postfix ? dynamic_cast<TokenDerefExpr *>(target) : NULL;
    if ( postfix_deref
      && dynamic_cast<TokenDerefStep *>(postfix_deref->expr) != NULL
      && postfix_deref->expr
      && postfix_deref->expr->datadef()
      && postfix_deref->expr->datadef()->is_pointer()
      && postfix_deref->deref_type
      && !postfix_deref->deref_type->is_pointer() )
    {
	// `*(*p++)++` parses as postfix++ wrapped around the final deref.
	// The actual increment target is the inner pointer lvalue `*p++`;
	// the surrounding `*` only dereferences the OLD pointer result.
	CompoundLHS lhs = resolveCompoundLHS(pgm, postfix_deref->expr, op_name);
	_operand = lhs.type->newreg(pgm.cc, old_name_hint);
	pgm.safemov(_operand, lhs.lval, lhs.type, lhs.type);
	apply_inc_dec_step(pgm, lhs.lval, lhs.type, increment, step);
	lhs.finish(pgm);
	if ( regdp.first )
	    pgm.safemov(*regdp.first, _operand, lhs.type, lhs.type);
	else
	    regdp.first = &_operand;
	regdp.second = lhs.type;
	return *regdp.first;
    }
    // Fast path: plain variable — apply step to its register/Mem directly.
    if ( target->type() == TokenType::ttVariable )
    {
	TokenVar *tv = dynamic_cast<TokenVar *>(target);
	Operand &reg = tv->operand(pgm);
	if ( reg.isMem() )
	{
	    if ( postfix )
	    {
		_operand = tv->var.type->newreg(pgm.cc, old_name_hint);
		pgm.safemov(_operand, reg, tv->var.type, tv->var.type);
		apply_inc_dec_step(pgm, reg, tv->var.type, increment, step);
	    }
	    else
	    {
		apply_inc_dec_step(pgm, reg, tv->var.type, increment, step);
		_operand = tv->var.type->newreg(pgm.cc, new_name_hint);
		pgm.safemov(_operand, reg, tv->var.type, tv->var.type);
	    }
	    if ( regdp.first )
		pgm.safemov(*regdp.first, _operand, tv->var.type, tv->var.type);
	    else
		regdp.first = &_operand;
	    tv->var.modified();
	    tv->putreg(pgm);
	    regdp.second = tv->var.type;
	    return *regdp.first;
	}
	if ( postfix )
	{
	    if ( regdp.first )
		pgm.safemov(*regdp.first, reg);
	    else
	    {
		_operand = tv->var.type->newreg(pgm.cc, old_name_hint);
		pgm.safemov(_operand, reg);
		regdp.first = &_operand;
	    }
	    apply_inc_dec_step(pgm, reg, tv->var.type, increment, step);
	}
	else
	{
	    apply_inc_dec_step(pgm, reg, tv->var.type, increment, step);
	    if ( regdp.first )
		pgm.safemov(*regdp.first, reg);
	    else
		regdp.first = &reg;
	}
	tv->var.modified();
	tv->putreg(pgm);
	regdp.second = tv->var.type;
	return *regdp.first;
    }

    // Member (obj->field / obj.field) or *deref: load → step → store.
    CompoundLHS lhs = resolveCompoundLHS(pgm, target, op_name);
    if ( postfix )
    {
	_operand = lhs.type->newreg(pgm.cc, old_name_hint);
	pgm.safemov(_operand, lhs.lval);
    }
    apply_inc_dec_step(pgm, lhs.lval, lhs.type, increment, step);
    lhs.finish(pgm);
    if ( !postfix )
	_operand = lhs.lval;  // stash new value on the node for pointer stability
    if ( regdp.first )
	pgm.safemov(*regdp.first, _operand);
    else
	regdp.first = &_operand;
    regdp.second = lhs.type;
    return *regdp.first;
}

/////////////////////////////////////////////////////////////////////////////
// compound assignment operators (+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)
// Pattern: resolve LHS, compile RHS normalized, apply safe op in place,
// write back + route result through finish_compound_assign.
/////////////////////////////////////////////////////////////////////////////

// Helper for +=, -=, *= (3-arg safe ops with DataDef slots).
static Operand &emit_compound_binop3(Program &pgm, TokenBase *caller,
				     TokenBase *left, TokenBase *right,
				     SafeBinOp3 op, regdefp_t &regdp, Operand &_operand,
				     const char *op_name)
{
    TokenVar *tv = dynamic_cast<TokenVar *>(left);
    if ( tv && tv->var.is_constant() )
	pgm.Throw(caller) << op_name << " on const variable '" << tv->var.name << "'" << flush;
    // C semantics: evaluate RHS before reading LHS value.
    DataDef *lhs_raw_type = compound_lhs_type(pgm, left);
    DataDef *op_type = promote_c_integer_type(lhs_raw_type);
    Operand tmp;
    Operand *out = regdp.first;
    regdp.second = op_type;
    regdp.first  = nullptr;
    Operand &rval = compile_compound_rhs_normalized(pgm, right, op_type, tmp);
    CompoundLHS lhs = resolveCompoundLHS(pgm, left, op_name);
    // Pointer arithmetic scaling: `p += n` must scale n by sizeof(*p).
    if ( lhs.type && lhs.type->is_pointer() && rval.isReg()
      && rval.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(lhs.type);
	if ( pdd && pdd->base_type && pdd->base_type->size > 1 )
	    pgm.cc.imul(rval.as<x86::Gp>(), rval.as<x86::Gp>(),
			imm((int64_t)pdd->base_type->size));
    }
    if ( op_type != lhs.type && lhs.lval.isReg() )
    {
	// Widen LHS value to operation width before the binop.
	x86::Gp wide = pgm.cc.newGpq("_cmpd_wide");
	pgm.safemov(wide, lhs.lval, op_type, lhs.type);
	(pgm.*op)(*(Operand *)&wide, rval, op_type, nullptr);
	pgm.safemov(lhs.lval, wide, lhs.type, op_type);
    }
    else
    {
	(pgm.*op)(lhs.lval, rval, op_type, nullptr);
    }
    return finish_compound_assign(pgm, regdp, lhs, _operand, out);
}

// Helper for |=, &=, ^=, <<=, >>= (2-arg bit/shift safe ops; right side is
// forced to a Gp).
static Operand &emit_compound_bitop2(Program &pgm, TokenBase *left, TokenBase *right,
				     SafeBitOp2 op, regdefp_t &regdp, Operand &_operand,
				     const char *op_name)
{
    if ( try_emit_large_simd_compound_bitop(pgm, left, right, op, regdp, _operand) )
	return _operand;
    // C semantics: evaluate RHS before reading LHS value, so side effects
    // from the RHS (e.g. `x[0] |= foo()` where foo modifies x[0]) are
    // visible when the LHS is read. Determine LHS type first for RHS
    // normalization, then compile RHS, then resolve (read) LHS.
    DataDef *lhs_type = compound_lhs_type(pgm, left);
    Operand tmp;
    Operand *out = regdp.first;
    regdp.second = lhs_type;
    regdp.first  = nullptr;
    Operand &rval = compile_compound_rhs_gp_normalized(pgm, right, lhs_type, tmp);
    CompoundLHS lhs = resolveCompoundLHS(pgm, left, op_name);
    (pgm.*op)(lhs.lval, rval, lhs.type);
    return finish_compound_assign(pgm, regdp, lhs, _operand, out);
}

// Helper for /= (return_remainder=false) and %= (return_remainder=true).
// safediv needs three distinct Gp regs, so the LHS value gets copied into
// a fresh dividend Reg and the result is stored back to lhs.lval.
static Operand &emit_compound_divmod(Program &pgm, TokenBase *left, TokenBase *right,
				     bool return_remainder, regdefp_t &regdp,
				     Operand &_operand, const char *op_name)
{
    // C semantics: evaluate RHS before reading LHS value.
    DataDef *lhs_raw_type = compound_lhs_type(pgm, left);
    DataDef *op_type = promote_c_integer_type(lhs_raw_type);
    Operand divisor;
    Operand *out = regdp.first;
    regdp.second = op_type;
    regdp.first  = nullptr;
    compile_compound_rhs_normalized(pgm, right, op_type, divisor);
    CompoundLHS lhs = resolveCompoundLHS(pgm, left, op_name);
    Operand dividend  = op_type->newreg(pgm.cc, "dividend");
    Operand remainder = op_type->newreg(pgm.cc, "remainder");
    pgm.safemov(dividend, lhs.lval, op_type, lhs.type);
    if ( op_type->is_integer() )
	pgm.safexor(remainder, remainder);
    pgm.safediv(remainder, dividend, divisor, op_type, op_type, op_type);
    Operand &result = return_remainder ? remainder : dividend;
    pgm.safemov(lhs.lval, result, lhs.type, op_type);
    return finish_compound_assign(pgm, regdp, lhs, _operand, out);
}

// Set up regdp.first as a scratch Reg of regdp.second when the caller
// either didn't pass a dest or passed a non-Reg dest (typically a Mem
// location we'll need to write back to at the end). Mirrors the
// pre-amble each binary-op general path used to open-code.
static GeneralBinopCascade begin_general_binop(Program &pgm, regdefp_t &regdp,
					       Operand &operand_storage)
{
    GeneralBinopCascade c;
    c.caller_dest = regdp.first;
    c.mirror_to_caller = c.caller_dest && !c.caller_dest->isReg();
    if ( !regdp.first || c.mirror_to_caller )
    {
	operand_storage = regdp.second->newreg(pgm.cc, "_reg");
	regdp.first = &operand_storage;
    }
    return c;
}

// After the safe* op has run on lval (which is the scratch set up
// above or the caller-passed Reg), mirror into the original caller
// Mem dest if we had one, then restore regdp.first to point at lval.
static Operand &finish_general_binop(Program &pgm, regdefp_t &regdp,
				     Operand &lval, GeneralBinopCascade &c)
{
    if ( c.mirror_to_caller )
	pgm.safemov(*c.caller_dest, lval, regdp.second, regdp.second);
    regdp.first = &lval;
    return *regdp.first;
}

static bool is_plain_numeric_expr(TokenBase *token)
{
    if ( !token ) return false;
    DataDef *dt = token->datadef();
    if ( !dt ) return false;
    if ( dt->is_pointer() ) return false;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(token) )
    {
	if ( tv->var.is_fixed_array() )
	    return false;
    }
    return dt->is_numeric();
}

void streamout_string(std::ostream &os, std::string &s)
{
//  DBG(std::cout << "streamout_string: << " << (uint64_t)&s << std::endl);
    os << s;
}

void streamout_cstr(std::ostream &os, const char *s)
{
    os << (s ? s : "(null)");
}

template<typename T> void streamout_numeric(std::ostream &os, T i)
{
//  DBG(std::cout << "streamout_numeric: sizeof(i) " << sizeof(i) << std::endl);
    os << i;
}

// istream >> string (read one word)
void *streamin_string(void *stream, void *str)
{
    *(std::istream *)stream >> *(std::string *)str;
    return stream;
}

// istream >> int
void *streamin_int(void *stream, void *val)
{
    *(std::istream *)stream >> *(int64_t *)val;
    return stream;
}

// stream input helpers for double (streamin_string/streamin_int already exist above)
void *streamin_double(void *stream, void *val)
{
    *(std::istream *)stream >> *(double *)val;
    return stream;
}

Operand &TokenInc::compile(Program &pgm, regdefp_t &regdp)
{
    TokenBase *target = left ? left : right;
    bool postfix = (left != nullptr);
    if ( !target ) pgm.Throw(this) << "Invalid increment" << flush;
    return emit_inc_dec(pgm, this, target, postfix, &Program::safeinc, regdp, _operand,
			"postinc", "preinc", "++");
}

Operand &TokenDec::compile(Program &pgm, regdefp_t &regdp)
{
    TokenBase *target = left ? left : right;
    bool postfix = (left != nullptr);
    if ( !target ) pgm.Throw(this) << "Invalid decrement" << flush;
    return emit_inc_dec(pgm, this, target, postfix, &Program::safedec, regdp, _operand,
			"postdec", "predec", "--");
}

Operand &TokenAddEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "+= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "+= missing rval operand" << flush;
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_addsub(pgm, left, right, /*subtract=*/false, regdp, _operand);
    return emit_compound_binop3(pgm, this, left, right, &Program::safeadd, regdp, _operand, "+=");
}

Operand &TokenSubEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "-= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "-= missing rval operand" << flush;
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_addsub(pgm, left, right, /*subtract=*/true, regdp, _operand);
    return emit_compound_binop3(pgm, this, left, right, &Program::safesub, regdp, _operand, "-=");
}

Operand &TokenMulEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "*= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "*= missing rval operand" << flush;
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_muldiv(pgm, left, right, /*divide=*/false, regdp, _operand);
    return emit_compound_binop3(pgm, this, left, right, &Program::safemul, regdp, _operand, "*=");
}

// Check that the left-hand side of a compound assignment is not const
#define CHECK_CONST_LHS(op_str) \
    { TokenVar *_tv = dynamic_cast<TokenVar *>(left); \
      if ( _tv && _tv->var.is_constant() ) \
          pgm.Throw(this) << op_str " on const variable '" << _tv->var.name << "'" << flush; }

Operand &TokenDivEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "/= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "/= missing rval operand" << flush;
    CHECK_CONST_LHS("/=")
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_muldiv(pgm, left, right, /*divide=*/true, regdp, _operand);
    return emit_compound_divmod(pgm, left, right, /*return_remainder=*/false, regdp, _operand, "/=");
}

Operand &TokenModEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "%= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "%= missing rval operand" << flush;
    CHECK_CONST_LHS("%=")
    return emit_compound_divmod(pgm, left, right, /*return_remainder=*/true, regdp, _operand, "%=");
}

Operand &TokenBSLEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "<<= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "<<= missing rval operand" << flush;
    CHECK_CONST_LHS("<<=")
    return emit_compound_bitop2(pgm, left, right, &Program::safeshl, regdp, _operand, "<<=");
}

Operand &TokenBSREq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << ">>= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << ">>= missing rval operand" << flush;
    CHECK_CONST_LHS(">>=")
    return emit_compound_bitop2(pgm, left, right, &Program::safeshr, regdp, _operand, ">>=");
}

Operand &TokenBandEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "&= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "&= missing rval operand" << flush;
    CHECK_CONST_LHS("&=")
    return emit_compound_bitop2(pgm, left, right, &Program::safeand, regdp, _operand, "&=");
}

Operand &TokenBorEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "|= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "|= missing rval operand" << flush;
    CHECK_CONST_LHS("|=")
    return emit_compound_bitop2(pgm, left, right, &Program::safeor, regdp, _operand, "|=");
}

Operand &TokenXorEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "^= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "^= missing rval operand" << flush;
    CHECK_CONST_LHS("^=")
    return emit_compound_bitop2(pgm, left, right, &Program::safexor, regdp, _operand, "^=");
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

    // multi-return: a, b := func()
    if ( !multi_vars.empty() )
    {
	DBG(pgm.cc.comment("multi-return assignment"));
	// allocate stack buffer for return values
	size_t bufsize = multi_vars.size() * 8;
	x86::Mem retbuf = pgm.cc.newStack((uint32_t)bufsize, 8);
	x86::Gp retbuf_ptr = pgm.cc.newIntPtr("__retbuf_ptr");
	pgm.cc.lea(retbuf_ptr, retbuf);

	// The right side is a function call — we need to inject retbuf_ptr as arg 0
	// Compile the call with the retbuf as the object pointer
	regdefp_t callrdp = {NULL, NULL, NULL};
	callrdp.object = &_operand; // temporary, will be overwritten
	_operand = retbuf_ptr;
	callrdp.object = &_operand;
	right->compile(pgm, callrdp);

	// after call, load each value from retbuf into the corresponding variable
	for ( size_t i = 0; i < multi_vars.size(); ++i )
	{
	    Operand &var_op = pgm.tkFunction->voperand(pgm, multi_vars[i]);
	    if ( multi_vars[i]->type->is_integer() || multi_vars[i]->type->is_numeric() )
	    {
		x86::Gp tmp = pgm.cc.newGpq("__mret_val");
		pgm.cc.mov(tmp, x86::qword_ptr(retbuf_ptr, (int32_t)(i * 8)));
		store_gp_to_var(pgm, tmp, var_op);
	    }
	    else if ( multi_vars[i]->type->is_string() )
	    {
		// string pointer stored in retbuf — copy to target string
		x86::Gp src_ptr = pgm.cc.newIntPtr("__mret_str");
		pgm.cc.mov(src_ptr, x86::qword_ptr(retbuf_ptr, (int32_t)(i * 8)));
		InvokeNode *scall;
		pgm.cc.invoke(&scall, imm(string_assign), FuncSignature::build<void, void *, void *>());
		scall->setArg(0, as_gp_ptr(pgm, var_op, "mret_dst"));
		scall->setArg(1, src_ptr);
	    }
	}

	regdp.first = &_operand;
	regdp.second = multi_vars[0]->type;
	return *regdp.first;
    }

    // handle variable token
    if ( left->type() == TokenType::ttVariable )
    {
	tvl = dynamic_cast<TokenVar *>(left);
	if ( tvl->var.is_constant() )
	    pgm.Throw(this) << "assignment to const variable '" << tvl->var.name << "'" << flush;
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
    if ( dynamic_cast<TokenComplexPart *>(left) )
    {
	TokenComplexPart *tcp = dynamic_cast<TokenComplexPart *>(left);
	ltype = tcp->datadef();
	_operand = tcp->operand(pgm);
	if ( regdp.second && regdp.second != ltype
	&&  !regdp.second->is_compatible(*ltype) )
	{
	    cerr << "regdp.second->type() " << (int)regdp.second->type() << " name: " << regdp.second->name << endl;
	    cerr << "     complex-part type() " << (int)ltype->type() << " name: " << ltype->name << endl;
	    throw "incompatible assignment";
	}
    }
    else
    // handle *ptr dereference on LHS
    if ( dynamic_cast<TokenDeref *>(left) )
    {
	TokenDeref *tdl = dynamic_cast<TokenDeref *>(left);
	ltype = tdl->deref_type;
	DBG(cout << "TokenAssign::compile() dereference assignment to *" << tdl->var.name << " type " << ltype->name << endl);
	_operand = tdl->operand(pgm);  // Mem operand [ptr]
    }
    else
    // handle *(expr) dereference on LHS (e.g. `**pp = v;` where the inner
    // `*pp` is a TokenDeref wrapped by a TokenDerefExpr, or any other
    // pointer-producing expression dereferenced as a write target).
    // TokenDerefExpr::type() returns ttMember, so without this branch we'd
    // fall through to the ttMember path and SIGSEGV at tml->var.type because
    // dynamic_cast<TokenMember *> returns NULL.
    if ( dynamic_cast<TokenDerefExpr *>(left) )
    {
	TokenDerefExpr *tdxl = dynamic_cast<TokenDerefExpr *>(left);
	ltype = tdxl->deref_type;
	DBG(cout << "TokenAssign::compile() dereference-expr assignment type " << ltype->name << endl);
	_operand = tdxl->operand(pgm);  // Mem operand [addr]
    }
    else
    // handle *ptr++ / *ptr-- on LHS (store through the current pointer
    // value, then advance/rewind the pointer variable itself). The read
    // side lives in TokenDerefStep::compile; the write side mirrors it:
    // capture old_ptr, step ptr, then expose [old_ptr] as the Mem lval
    // that the numeric-assignment path writes into.
    if ( dynamic_cast<TokenDerefStep *>(left) )
    {
	TokenDerefStep *tdsl = dynamic_cast<TokenDerefStep *>(left);
	ltype = tdsl->deref_type;
	DBG(cout << "TokenAssign::compile() *ptr"
	    << (tdsl->increment ? "++" : "--")
	    << " store type " << (ltype ? ltype->name : "?") << endl);
	Operand &ptr_op = pgm.tkFunction->voperand(pgm, &tdsl->var);
	if ( !ptr_op.isReg() || !ptr_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	    throw "TokenDerefStep LHS: pointer operand is not a gp register";
	x86::Gp ptr_reg = ptr_op.as<x86::Gp>();
	x86::Gp old_ptr = pgm.cc.newIntPtr(tdsl->increment ? "store_postinc_ptr" : "store_postdec_ptr");
	pgm.safemov(old_ptr, ptr_reg);
	int step = ltype ? (int)ltype->size : 1;
	if ( step <= 0 ) step = 1;
	if ( tdsl->increment )
	    pgm.safeadd(ptr_reg, step, tdsl->var.type, ltype);
	else
	    pgm.safesub(ptr_reg, step, tdsl->var.type, ltype);
	tdsl->var.modified();
	_operand = x86::ptr(old_ptr, 0, (uint32_t)ltype->size);
    }
    else
    // handle member token (struct.member or ptr->member)
    if ( left->type() == TokenType::ttMember )
    {
	tml = dynamic_cast<TokenMember *>(left);
	ltype = tml->var.type;
	if ( const DataDefSTRUCT::BitFieldInfo *bf = tml->bitfield_info() )
	{
	    DBG(pgm.cc.comment("TokenAssign::compile() bit-field assignment"));
	    Operand &storage_op = tml->operand(pgm);
	    if ( !storage_op.isMem() )
		pgm.Throw(left) << "Assignment to bit-field with non-memory storage" << flush;

	    regdefp_t rhs_rdp = {NULL, NULL, NULL};
	    rhs_rdp.second = ltype;
	    Operand &rhs_op = right->compile(pgm, rhs_rdp);
	    x86::Mem storage = storage_op.as<x86::Mem>();
	    storage.setSize((uint32_t)bf->storage_size);
	    x86::Gp assigned = emit_bitfield_store_operand(pgm, storage, *bf,
		rhs_op, rhs_rdp.second ? rhs_rdp.second : right->datadef(),
		ltype, "assign_bf");

	    _operand = assigned;
	    regdp.second = ltype;
	    if ( regdp.first && regdp.first != &_operand )
	    {
		pgm.safemov(*regdp.first, _operand, ltype, ltype);
		return *regdp.first;
	    }
	    regdp.first = &_operand;
	    return _operand;
	}
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
    if ( left->type() == TokenType::ttSubscript )
    {
	// subscript write: container[index] = value
	TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(left);
	if ( !tsub )
	{
	    // TokenSubscriptExpr (subscript with an expression base like
	    // `s->items[i] = v`) reports ttSubscript too but isn't a
	    // TokenSubscript. Emit the element store directly: compile
	    // base_expr for the pointer, compile index, then store rhs at
	    // [base + idx * elem_size].
	    TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(left);
	    if ( !tse )
		throw "TokenAssign: unknown subscript lvalue type";
	    DataDef *elem_type = tse->datadef();
	    size_t elem_size = elem_type ? elem_type->size : 8;

	    // Compile RHS first so the caller's destination (if any) is
	    // preserved through the LHS evaluation.
	    regdefp_t rhs_rdp = {NULL, NULL, NULL};
	    rhs_rdp.second = elem_type;
	    Operand &rhs_op = right->compile(pgm, rhs_rdp);
	    Operand effective_rhs = rhs_op;
	    if ( type_is_cstr_pointer(elem_type)
	      && right->datadef()
	      && right->datadef()->rawtype() == DataType::dtSTRING )
	    {
		x86::Gp cstr = pgm.cc.newIntPtr("subx_cstr");
		InvokeNode *cstr_call;
		pgm.cc.invoke(&cstr_call, imm(string_cstr),
		    FuncSignature::build<const char *, void *>());
		cstr_call->setArg(0, as_gp_ptr(pgm, rhs_op, "sub_strptr"));
		cstr_call->setRet(0, cstr);
		effective_rhs = cstr;
	    }

	    // Use operand() rather than compile() here: for an aggregate
	    // base_expr (e.g. a struct-contained `int bits[4]` exposed as
	    // TokenMember with a numeric _datatype), compile() would emit
	    // the load-first-element-into-Gp path via emit_ir_value, and we'd
	    // end up indexing off the element value instead of the array
	    // base address. operand() returns the raw Mem/Gp.
	    bool base_needs_compile =
		dynamic_cast<TokenAdd *>(tse->base_expr) != NULL
	     || dynamic_cast<TokenSub *>(tse->base_expr) != NULL
	     || dynamic_cast<TokenAssign *>(tse->base_expr) != NULL
	     || dynamic_cast<TokenCast *>(tse->base_expr) != NULL;
	    Operand base_storage;
	    Operand *base_ptr;
	    if ( base_needs_compile )
	    {
		regdefp_t base_rdp = {nullptr, tse->base_expr->datadef(), nullptr};
		base_ptr = &tse->base_expr->compile(pgm, base_rdp);
	    }
	    else
		base_ptr = &tse->base_expr->operand(pgm);
	    Operand &base_op = *base_ptr;
	    x86::Gp base_reg = pgm.cc.newIntPtr("subx_base");
	    if ( base_op.isReg() && base_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.mov(base_reg, base_op.as<x86::Gp>());
	    else if ( base_op.isMem() )
	    {
		// If base_expr is a pointer-typed value (e.g. `s->items` where
		// items is `int *`), the Mem holds the pointer value — load it
		// with MOV to get the real array address. If base_expr is an
		// aggregate stored in-place (struct-contained fixed array, local
		// array), we want the address OF the Mem — LEA gives that.
		DataDef *bdd = tse->base_expr->datadef();
		if ( bdd && bdd->is_pointer() && !is_fixed_array_struct_member(tse->base_expr) )
		    pgm.cc.mov(base_reg, base_op.as<x86::Mem>());
		else
		    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
	    }
	    else
		throw "TokenAssign: subscript base is not a register or memory";

	    regdefp_t idx_rdp = {NULL, NULL, NULL};
	    Operand &idx_op = tse->index->compile(pgm, idx_rdp);
	    x86::Gp idx_reg = pgm.cc.newGpq("subx_idx");
	    load_idx_to_gpq(pgm, idx_reg, idx_op);

	    // SIB scale only covers 1/2/4/8 — for any other element size (e.g.
	    // sizeof(struct K) == 16) fold the element stride into the index
	    // via imul and access with scale = 1.
	    uint32_t shift = scale_index_by_element_size(pgm, idx_reg, elem_type, "subx_size");
	    x86::Mem slot = x86::ptr(base_reg, idx_reg, shift, 0, (uint32_t)elem_size);

	    if ( elem_type && (elem_type->basetype() == BaseType::btStruct
			    || elem_type->basetype() == BaseType::btClass) )
	    {
		Operand dst_slot = slot;
		emit_raw_aggregate_copy(pgm, dst_slot, effective_rhs, elem_type, "subx_struct_copy");
		_operand = slot;
		regdp.first = &_operand;
		regdp.second = elem_type;
		return _operand;
	    }
	    {
		IRBuilder ir(pgm.cc);
		DataDef *rhs_type = rhs_rdp.second ? rhs_rdp.second : elem_type;
		ir.store(IRValue::mem(slot, elem_type),
			 ir_from_operand(effective_rhs, rhs_type));
	    }

	    // The expression value of `arr[i] = x` is the LHS-typed value —
	    // truncated and (sign- / zero-) extended.  Re-load from the
	    // slot we just wrote so safemov picks the right movsx / movzx.
	    Operand expr_value = effective_rhs;
	    if ( elem_type && elem_type->is_integer() && elem_type->size < 8 )
	    {
		x86::Gp expr_val = pgm.cc.newGpq("subx_expr");
		pgm.safemov(expr_val, slot, &ddINT64, elem_type);
		expr_value = expr_val;
	    }
	    if ( regdp.first && regdp.first != &_operand )
	    {
		pgm.safemov(*regdp.first, expr_value, elem_type, elem_type);
		regdp.second = elem_type;
		_operand = *regdp.first;
		return *regdp.first;
	    }
	    _operand = expr_value;
	    regdp.first = &_operand;
	    regdp.second = elem_type;
	    return _operand;
	}
	ltype = tsub->datadef();
	DBG(cout << "TokenAssign::compile() subscript assignment to " << tsub->object.name << " elem type " << ltype->name << endl);
	DBG(pgm.cc.comment("TokenAssign: subscript write"));
	// compile right side independently
	regdefp_t rhs_rdp = {nullptr, nullptr, nullptr};
	if ( !ltype->is_complex() )
	    rhs_rdp.second = ltype;
	Operand &rhs_op = right->compile(pgm, rhs_rdp);
	// Coerce dtSTRING → char*: storing a string literal into a
	// `char *arr[N]` slot used to write the std::string object's
	// address into the slot (not its c_str()). Match the same
	// coercion TokenAssign applies for plain `char *p = "literal";`.
	Operand effective_rhs = rhs_op;
	if ( type_is_cstr_pointer(ltype)
	  && right->datadef() && right->datadef()->rawtype() == DataType::dtSTRING )
	{
	    x86::Gp cstr = pgm.cc.newIntPtr("sub_cstr");
	    InvokeNode *cstr_call;
	    pgm.cc.invoke(&cstr_call, imm(string_cstr),
		FuncSignature::build<const char *, void *>());
	    cstr_call->setArg(0, as_gp_ptr(pgm, rhs_op, "sub_strptr"));
	    cstr_call->setRet(0, cstr);
	    effective_rhs = cstr;
	}
	tsub->compile_set(pgm, effective_rhs, rhs_rdp.second ? rhs_rdp.second : ltype);
	// The expression value of `arr[i] = x` is the LHS-typed value —
	// truncated to LHS width and (sign- / zero-) extended back to int64.
	// For wider or non-integer LHS the RHS is already correctly typed.
	// Without this, `(buf[i] = c) != EOF` and `r = (buf[i] = X)` see
	// the unbound RHS register's full int.
	Operand expr_value = effective_rhs;
	if ( ltype && ltype->is_integer() && ltype->size < 8
	  && effective_rhs.isReg()
	  && effective_rhs.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    x86::Gp expr_val = pgm.cc.newGpq("subassign_expr");
	    x86::Gp src = effective_rhs.as<x86::Gp>();
	    bool is_unsigned = ltype->is_unsigned();
	    if ( ltype->size == 1 )
	    {
		if ( is_unsigned ) pgm.cc.movzx(expr_val.r32(), src.r8());
		else               pgm.cc.movsx(expr_val, src.r8());
	    }
	    else if ( ltype->size == 2 )
	    {
		if ( is_unsigned ) pgm.cc.movzx(expr_val.r32(), src.r16());
		else               pgm.cc.movsx(expr_val, src.r16());
	    }
	    else /* size == 4 */
	    {
		if ( is_unsigned ) pgm.cc.mov(expr_val.r32(), src.r32());
		else               pgm.cc.movsxd(expr_val, src.r32());
	    }
	    expr_value = expr_val;
	}
	// If the caller passed a destination via regdp.first (e.g. an outer
	// `r = (buf[i] = x)` set it to r's storage, or `(buf[i] = c) != EOF`
	// passed the comparison's tmp Gp), write the expression value there
	// directly.  Otherwise expose it via _operand for consumers that
	// read regdp's pair.
	if ( regdp.first && regdp.first != &_operand )
	{
	    pgm.safemov(*regdp.first, expr_value, ltype, ltype);
	    regdp.second = ltype;
	    _operand = *regdp.first;
	    return *regdp.first;
	}
	_operand = expr_value;
	regdp.first = &_operand;
	regdp.second = ltype;
	return _operand;
    }
    else
    {
	// *(*p)++ = c  —  post-increment wrapping a dereference-expression.
	// The parser produces TokenInc(TokenDerefExpr(inner)) because postfix
	// ++ binds after the outer unary *.  Semantics: capture old pointer,
	// step the pointer, store rhs at [old_ptr].
	TokenOperator *inc_dec = dynamic_cast<TokenOperator *>(left);
	TokenDerefExpr *inner_deref = inc_dec
	    ? dynamic_cast<TokenDerefExpr *>(inc_dec->right ? inc_dec->right : inc_dec->left)
	    : NULL;
	if ( inner_deref && (left->id() == TokenID::tkInc || left->id() == TokenID::tkDec) )
	{
	    // Reuse the same lvalue materialization used by standalone
	    // `*p++` / `(*p++)++`: for `*(*p++)++ = rhs`, the increment
	    // target is the pointer lvalue produced by the inner `*p++`,
	    // not the final `*(*p++)` int lvalue.
	    CompoundLHS ptr_lhs = resolveCompoundLHS(pgm, inner_deref->expr, "deref-inc assign");
	    ltype = inner_deref->deref_type;
	    x86::Gp ptr_val = ptr_lhs.lval.as<x86::Gp>();
	    // Save old pointer value for the store target
	    x86::Gp old_ptr = pgm.cc.newIntPtr("deref_inc_old");
	    pgm.cc.mov(old_ptr, ptr_val);
	    // Post-increment/decrement
	    int step = ltype ? (int)ltype->size : 1;
	    if ( step <= 0 ) step = 1;
	    if ( left->id() == TokenID::tkInc )
		pgm.cc.add(ptr_val, imm(step));
	    else
		pgm.cc.sub(ptr_val, imm(step));
	    // Write incremented value back to the pointer lvalue (`*p++`).
	    ptr_lhs.finish(pgm);
	    // The assignment target is [old_ptr]
	    _operand = x86::ptr(old_ptr, 0, ltype ? (uint32_t)ltype->size : 1);
	}
	else
	    pgm.Throw(this) << "Assignment on a non-variable lval" << flush;
    }

    if ( !regdp.first || !regdp.second )
    {
	// For narrow integer LHS (char/short), don't propagate the LHS type
	// to the RHS compilation — the RHS expression must apply C integer
	// promotions (e.g. `unsigned char x; x = x / -5;` must compute the
	// division in int, not in unsigned char where -5 wraps to 251).
	// The LHS type is applied only at the final store.
	if ( ltype->is_integer() && ltype->size < 4 )
	    ; // leave regdp.second = NULL → RHS infers its own type
	else
	    regdp.second = ltype;
    }

    // Post-declaration string-literal → char* assignment: `char *p; p =
    // "literal";`. The LHS is a char* (pointer, so is_numeric), but the
    // RHS is a dtSTRING object — passing the dtSTRING operand as
    // regdp.first would write the std::string pointer into p instead of
    // its data. Compile the RHS into a tmp, pass it through string_cstr,
    // then store the resulting char* into p's register.
    if ( type_is_cstr_pointer(ltype)
      && right->datadef() && right->datadef()->rawtype() == DataType::dtSTRING )
    {
	Operand rhs_storage;
	Operand &cstr = compile_token_normalized(pgm, right, ltype, nullptr, rhs_storage);
	pgm.safemov(_operand, cstr, ltype, ltype);
	if ( tvl ) { tvl->var.modified(); tvl->putreg(pgm); }
	regdp.first = &_operand;
	regdp.second = ltype;
	return *regdp.first;
    }

    // we should have _operand set to our left variable at this point
    // only if our left variable is numeric do we want to pass it to
    // our right side, otherwise we want to clear it
    if ( ltype->is_numeric() )
    {
	Operand *orig_operand = regdp.first;
	regdp.first = &_operand;
	r_operand = &right->compile(pgm, regdp);
	// Real → integer assignment: the RHS compiled with its natural
	// type (double/float) and may have stored raw float bits into the
	// integer LHS Mem via emit_ir_value. Fix by converting now.
	if ( ltype->is_integer() && regdp.second && regdp.second->is_real()
	  && _operand.isMem() )
	{
	    IRBuilder ir(pgm.cc);
	    // Reload the raw double from the Mem, convert to int, store back.
	    x86::Xmm tmp_xmm = (regdp.second->size == sizeof(float))
		? pgm.cc.newXmmSs("_r2i_tmp") : pgm.cc.newXmmSd("_r2i_tmp");
	    if ( regdp.second->size == sizeof(float) )
		pgm.cc.movss(tmp_xmm, _operand.as<x86::Mem>());
	    else
		pgm.cc.movsd(tmp_xmm, _operand.as<x86::Mem>());
	    x86::Gp tmp_gp = pgm.cc.newGpq("_r2i_gp");
	    if ( regdp.second->size == sizeof(float) )
		pgm.cc.cvttss2si(tmp_gp, tmp_xmm);
	    else
		pgm.cc.cvttsd2si(tmp_gp, tmp_xmm);
	    pgm.safemov(_operand.as<x86::Mem>(), tmp_gp, ltype, ltype);
	    regdp.second = ltype;
	}
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
	regdp.second = NULL;
	if ( (ltype->basetype() == BaseType::btStruct
	   || ltype->basetype() == BaseType::btClass)
	  && !ltype->is_complex() )
	    regdp.second = ltype;
	r_operand = &right->compile(pgm, regdp);
	regdp.first = orig_operand ? orig_operand : r_operand;
    }

    if ( ltype->is_complex() && regdp.second
      && (regdp.second->is_numeric() || regdp.second->is_complex()) )
    {
	DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(ltype);
	if ( !cdd )
	    throw "TokenAssign: expected complex datadef";
	IRBuilder ir(pgm.cc);
	Operand real_mem = _operand;
	if ( real_mem.isMem() )
	    real_mem.as<x86::Mem>().setSize((uint32_t)cdd->element_type->size);
	else
	    real_mem = x86::ptr(real_mem.as<x86::Gp>(), 0, (uint32_t)cdd->element_type->size);
	Operand imag_mem = _operand;
	if ( imag_mem.isMem() )
	{
	    imag_mem.as<x86::Mem>().addOffset((int64_t)cdd->component_offset(true));
	    imag_mem.as<x86::Mem>().setSize((uint32_t)cdd->element_type->size);
	}
	else
	    imag_mem = x86::ptr(imag_mem.as<x86::Gp>(),
				(int32_t)cdd->component_offset(true),
				(uint32_t)cdd->element_type->size);
	if ( regdp.second->is_complex() )
	{
	    DataDefCOMPLEX *src_cdd = dynamic_cast<DataDefCOMPLEX *>(regdp.second);
	    if ( !src_cdd || !src_cdd->element_type )
		throw "TokenAssign: expected complex rhs datadef";
	    Operand src_real = *r_operand;
	    if ( src_real.isMem() )
	    {
		src_real.as<x86::Mem>().setSize((uint32_t)src_cdd->element_type->size);
	    }
	    else
		src_real = x86::ptr(src_real.as<x86::Gp>(), 0, (uint32_t)src_cdd->element_type->size);
	    Operand src_imag = *r_operand;
	    if ( src_imag.isMem() )
	    {
		src_imag.as<x86::Mem>().addOffset((int64_t)src_cdd->component_offset(true));
		src_imag.as<x86::Mem>().setSize((uint32_t)src_cdd->element_type->size);
	    }
	    else
		src_imag = x86::ptr(src_imag.as<x86::Gp>(),
				(int32_t)src_cdd->component_offset(true),
				(uint32_t)src_cdd->element_type->size);
	    IRValue real_val = ir.load(ir.coerce(ir_from_operand(src_real, src_cdd->element_type),
						 cdd->element_type));
	    IRValue imag_val = ir.load(ir.coerce(ir_from_operand(src_imag, src_cdd->element_type),
						 cdd->element_type));
	    ir.store(IRValue::mem(real_mem.as<x86::Mem>(), cdd->element_type), real_val);
	    ir.store(IRValue::mem(imag_mem.as<x86::Mem>(), cdd->element_type), imag_val);
	}
	else
	{
	    ir.store(IRValue::mem(real_mem.as<x86::Mem>(), cdd->element_type),
		     ir_from_operand(*r_operand, regdp.second));
	    ir.store(IRValue::mem(imag_mem.as<x86::Mem>(), cdd->element_type),
		     IRValue::imm(Imm(0), &ddINT));
	}
	if ( tvl )
	{
	    tvl->var.modified();
	    tvl->putreg(pgm);
	}
	regdp.first = &_operand;
	regdp.second = ltype;
	return _operand;
    }

    if ( !regdp.second )
	throw "TokenAssign: no rval type";

    // Large SIMD assignment: memory-to-memory copy via qword iteration.
    // Can't go through safemov (which only handles 1/2/4/8-byte scalars).
    if ( is_large_simd_type(ltype) )
    {
	DBG(pgm.cc.comment("TokenAssign::compile() large SIMD assignment"));
	DataDefSIMD *vdd = static_cast<DataDefSIMD *>(ltype);
	// Compile RHS into a memory-backed slot
	regdefp_t rhs_rdp = {nullptr, vdd, nullptr};
	Operand &rhs_op = right->compile(pgm, rhs_rdp);
	x86::Mem src_mem = large_simd_expr_mem(pgm, right, vdd, "lsimd_assign_src");
	// Get LHS memory address
	x86::Gp dst_base = pgm.cc.newIntPtr("lsimd_dst");
	if ( _operand.isMem() )
	    pgm.cc.lea(dst_base, _operand.as<x86::Mem>());
	else
	    pgm.cc.mov(dst_base, _operand.as<x86::Gp>());
	x86::Gp src_base = pgm.cc.newIntPtr("lsimd_src");
	pgm.cc.lea(src_base, src_mem);
	// Qword-by-qword copy
	for ( size_t b = 0; b < vdd->size; b += 8 )
	{
	    x86::Gp tmp = pgm.cc.newGpq("lsimd_cp");
	    pgm.cc.mov(tmp, x86::ptr(src_base, (int32_t)b, 8));
	    pgm.cc.mov(x86::ptr(dst_base, (int32_t)b, 8), tmp);
	}
	if ( tvl )
	{
	    tvl->var.modified();
	    tvl->putreg(pgm);
	}
	regdp.first = &_operand;
	regdp.second = ltype;
	return _operand;
    }

    bool lhs_scalar = ltype->is_numeric() || ltype->is_pointer();
    bool rhs_scalar = regdp.second->is_numeric() || regdp.second->is_pointer();
    if ( lhs_scalar && !rhs_scalar )
	throw "Expecting rval to be numeric";
    if ( !ltype->is_integer() && !ltype->is_pointer() && regdp.second->is_integer() )
	throw "Not expecting rval to be numeric";
    if ( ltype->is_string() && !regdp.second->is_string() )
	throw "Expecting rval to be string";

    if ( lhs_scalar )
    {
	DBG(cout << "TokenAssign::compile() numeric to numeric" << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() numeric to numeric"));
	if ( tvl )
	{
	    tvl->var.modified();
	    tvl->putreg(pgm);
	}
	// C assignment expressions return the assigned value. If the caller
	// provided a destination (regdp.first set to something other than
	// our _operand — e.g. `(x = f()) < 25` where TokenLT passed a fresh
	// Gp), mirror _operand's value into it so the containing expression
	// can consume the assigned value.
	if ( regdp.first && regdp.first != &_operand )
	{
	    DBG(pgm.cc.comment("TokenAssign::compile() mirror to caller dest"));
	    pgm.safemov(*regdp.first, _operand, ltype, ltype);
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
	x86::Gp dst_arg = materialize_gp_ptr_arg(pgm, _operand, "str_dst");
	x86::Gp src_arg = materialize_gp_ptr_arg(pgm, *r_operand, "str_src");
        InvokeNode* call; pgm.cc.invoke(&call, imm(string_assign), FuncSignature::build<void, void *, void *>());
	call->setArg(0, dst_arg);
	call->setArg(1, src_arg);
	if ( tvl )
	{
	    tvl->var.modified();
	    tvl->putreg(pgm);
	}
	// String assignment expressions evaluate to the LHS object. Keep the
	// destination operand live and drop the RHS from the outward regdp
	// pair so call lowering does not preserve a stale source operand
	// across the assignment call.
	regdp.first = &_operand;
	regdp.second = ltype;
    }
    else
    if ( ltype->basetype() == BaseType::btStruct
      || ltype->basetype() == BaseType::btClass )
    {
	// Struct/class copy: emit direct chunked loads/stores instead of a
	// helper call. This preserves the existing raw-byte-copy semantics
	// while avoiding an AOT-only external-call edge for small fixed-size
	// copies like `T copy = *(T*)p;`.
	// Both sides expose their storage either as a Mem (stack slot for a
	// local struct variable) or as a Gp holding a LEA'd address (for
	// member-access shapes). LEA the Mem form into a Gp; use the Gp form
	// directly.
	if ( ltype != regdp.second )
	    throw "TokenAssign struct: lhs/rhs struct types differ";
	DBG(cout << "TokenAssign::compile() struct to struct (" << ltype->name << ", " << ltype->size << " bytes)" << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() struct memcpy"));
	emit_raw_aggregate_copy(pgm, _operand, *r_operand, ltype, "struct_copy");
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

// addition
Operand &TokenAdd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAdd::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "+ missing lval operand"; }
    if ( !right ) { throw "+ missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator+", regdp, _operand) )
	return _operand;
    DataDef *lptr_type = effective_pointer_type_for_arith(pgm, left);
    DataDef *rptr_type = effective_pointer_type_for_arith(pgm, right);
    if ( !lptr_type && !rptr_type
      && ((left->datadef() && left->datadef()->is_complex())
       || (right->datadef() && right->datadef()->is_complex())) )
	return emit_complex_addsub(pgm, left, right, /*subtract=*/false, regdp, _operand);
    if ( !lptr_type && !rptr_type && can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    if ( (lptr_type || rptr_type) && regdp.second && !regdp.second->is_pointer() )
	regdp.second = lptr_type ? lptr_type : rptr_type;
    settype(pgm, regdp);				 // set regdp.second type
    {
	bool real_ops = (left && left->is_real()) || (right && right->is_real());
	bool dest_int_real = real_ops && regdp.second && regdp.second->is_integer();
	if ( !dest_int_real && is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	    return emit_plain_binop3(pgm, left, right, regdp.second, &Program::safeadd, regdp, _operand, "_add_l");
    }
    if ( lptr_type && !rptr_type )
    {
	GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
	Operand &lval = compile_token_normalized(pgm, left, regdp.second,
						 regdp.first, _operand);
	regdp.first = &lval;
	if ( !regdp.second ) { throw "TokenAdd::compile() left->compile() cleared datatype!"; }
	Operand rhs_storage;
	DataDef *rhs_type = NULL;
	Operand &rval = compile_pointer_offset_operand(pgm, right, rhs_storage, rhs_type);
	emit_pointer_arith_scale(pgm, left, right, rval);
	pgm.safeadd(lval, rval, regdp.second);
	return finish_general_binop(pgm, regdp, lval, c);
    }
    if ( rptr_type && !lptr_type )
    {
	GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
	Operand &lval = compile_token_normalized(pgm, right, regdp.second,
						 regdp.first, _operand);
	regdp.first = &lval;
	if ( !regdp.second ) { throw "TokenAdd::compile() right->compile() cleared datatype!"; }
	Operand rhs_storage;
	DataDef *rhs_type = NULL;
	Operand &rval = compile_pointer_offset_operand(pgm, left, rhs_storage, rhs_type);
	emit_pointer_arith_scale(pgm, right, left, rval);
	pgm.safeadd(lval, rval, regdp.second);
	return finish_general_binop(pgm, regdp, lval, c);
    }
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenAdd::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    emit_pointer_arith_scale(pgm, left, right, rval);	 // p + n → p + n*sizeof(*p)
    pgm.safeadd(lval, rval, regdp.second);
    return finish_general_binop(pgm, regdp, lval, c);
}

// subtraction
Operand &TokenSub::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenSub::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "- missing lval operand"; }
    if ( !right ) { throw "- missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator-", regdp, _operand) )
	return _operand;
    DataDef *lptr_type = effective_pointer_type_for_arith(pgm, left);
    DataDef *rptr_type = effective_pointer_type_for_arith(pgm, right);
    if ( !lptr_type && !rptr_type
      && ((left->datadef() && left->datadef()->is_complex())
       || (right->datadef() && right->datadef()->is_complex())) )
	return emit_complex_addsub(pgm, left, right, /*subtract=*/true, regdp, _operand);
    if ( !lptr_type && can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    if ( lptr_type && regdp.second && !regdp.second->is_pointer() )
	regdp.second = lptr_type;
    settype(pgm, regdp);				 // set regdp.second type
    {
	bool real_ops = (left && left->is_real()) || (right && right->is_real());
	bool dest_int_real = real_ops && regdp.second && regdp.second->is_integer();
	if ( !dest_int_real && is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	    return emit_plain_binop3(pgm, left, right, regdp.second, &Program::safesub, regdp, _operand, "_sub_l");
    }
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenSub::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    emit_pointer_arith_scale(pgm, left, right, rval);	 // ptr - n → ptr - n*sizeof(*ptr)
    pgm.safesub(lval, rval, regdp.second);
    // ptr - ptr: C requires element count, not byte count.
    // Divide the raw byte difference by sizeof(element).
    {
	DataDef *ltype = effective_pointer_type_for_arith(pgm, left);
	DataDef *rtype = effective_pointer_type_for_arith(pgm, right);
	if ( ltype && rtype )
	{
	    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(ltype);
	    size_t elem_size = pdd && pdd->base_type ? pdd->base_type->size : 0;
	    if ( elem_size > 1 && lval.isReg() )
	    {
		// Use SAR for power-of-2 element sizes (common case),
		// otherwise emit a full idiv sequence.
		x86::Gp diff = lval.as<x86::Gp>();
		bool is_pow2 = (elem_size & (elem_size - 1)) == 0;
		if ( is_pow2 )
		{
		    int shift = 0;
		    for (size_t s = elem_size; s > 1; s >>= 1) ++shift;
		    pgm.cc.sar(diff, shift);
		}
		else
		{
		    x86::Gp divisor = pgm.cc.newGpq("_ptrdiff_d");
		    pgm.cc.mov(divisor, (int64_t)elem_size);
		    // x86 idiv: sign-extend diff into rdx:rax, divide
		    x86::Gp rdx_hi = pgm.cc.newGpq("_ptrdiff_hi");
		    pgm.cc.mov(rdx_hi, diff);
		    pgm.cc.sar(rdx_hi, 63);
		    pgm.cc.idiv(rdx_hi, diff, divisor);
		}
		regdp.second = &ddINT64;
	    }
	}
    }
    return finish_general_binop(pgm, regdp, lval, c);
}

// make number negative
Operand &TokenNeg::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenNeg::Compile() TOP" << endl);
    if ( !right ) { throw "- missing rval operand"; }
    settype(pgm, regdp);				 // set regdp.second type
    // TokenNeg is unary; `left` is structurally NULL, so the old
    // `is_plain_numeric_expr(left) && is_plain_numeric_expr(right)`
    // plain-numeric fast path was unreachable — and its body wrongly
    // emitted safeshl instead of safeneg. Replace it with a real
    // unary-right fast path that normalizes through the IR.
    if ( is_plain_numeric_expr(right) && regdp.second && regdp.second->is_numeric() )
    {
	Operand *caller_dest = regdp.first;
	Operand rval_reg = regdp.second->newreg(pgm.cc, "_neg");
	Operand rval_norm;
	Operand &rval = compile_token_normalized(pgm, right, regdp.second, &rval_reg, rval_norm);
	pgm.safeneg(rval, regdp.second);
	_operand = rval;
	regdp.first = &_operand;
	if ( caller_dest )
	{
	    regdp.first = caller_dest;
	    return emit_ir_value(pgm, IRValue::reg(rval, regdp.second), regdp, _operand, regdp.second);
	}
	return _operand;
    }
    Operand *caller_dest = regdp.first;
    bool mirror_to_caller = caller_dest && !caller_dest->isReg();
    if ( !regdp.first || mirror_to_caller )		 // if not passed a usable register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &rval = right->compile(pgm, regdp);		 // compile right side ref=rval
    if ( !regdp.second ) { throw "TokenNeg::compile() right->compile cleared datatype"; }
    pgm.safeneg(rval, regdp.second);			 // type safe negation
    if ( mirror_to_caller )				 // write result back to caller's Mem
	pgm.safemov(*caller_dest, rval, regdp.second, regdp.second);
    regdp.first = &rval;				 // restore regdp.first
    return *regdp.first;				 // return result operand
}

// multiply two numbers
Operand &TokenMul::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMul::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "* missing lval operand"; }
    if ( !right ) { throw "* missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator*", regdp, _operand) )
	return _operand;
    if ( (left->datadef() && left->datadef()->is_complex())
      || (right->datadef() && right->datadef()->is_complex()) )
	return emit_complex_muldiv(pgm, left, right, /*divide=*/false, regdp, _operand);
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    // Skip the plain-binop fast path when operands are mixed int/real —
    // emit_plain_binop3 would force both to the destination type before
    // the multiply, truncating the float. The general path below handles
    // promotion per C99 usual arithmetic conversions.  Also skip when
    // destination type is integer but any operand is real — the multiply
    // must compute in float and convert to int afterward, not truncate
    // the float literal to int before multiplying.
    bool any_real = (left && left->is_real()) || (right && right->is_real());
    bool mixed_real = (left && left->is_real()) != (right && right->is_real());
    bool dest_int_but_real_ops = any_real && regdp.second && regdp.second->is_integer();
    // SIMD scalar-to-vector: compile_token_normalized handles the splat,
    // so mixed int/float is fine when the destination is SIMD.
    bool dest_is_simd = regdp.second && regdp.second->is_simd();
    if ( (dest_is_simd || (!mixed_real && !dest_int_but_real_ops)) && is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	return emit_plain_binop3(pgm, left, right, regdp.second, &Program::safemul, regdp, _operand, "_mul_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    // When the result type is real but the LHS is a bitwise operator,
    // compile the LHS at its natural integer type and convert. Bitwise
    // operators produce integer Gp results; passing them a double
    // target makes them attempt Xmm mode, producing zeroes.
    bool lhs_bitwise = left && left->is_operator()
	&& (left->id() == TokenID::tkBand || left->id() == TokenID::tkBor
	 || left->id() == TokenID::tkXor || left->id() == TokenID::tkBSL
	 || left->id() == TokenID::tkBSR);
    if ( lhs_bitwise && regdp.second && regdp.second->is_real() )
    {
	regdefp_t lhs_rdp = {nullptr, nullptr, nullptr};
	Operand &lval_int = left->compile(pgm, lhs_rdp);
	DataDef *ltype_int = lhs_rdp.second ? lhs_rdp.second : &ddINT;
	// Convert int → double
	IRBuilder ir(pgm.cc);
	IRValue converted = ir.coerce(ir_from_operand(lval_int, ltype_int), regdp.second);
	converted = ir.load(converted);
	Operand lval = converted.op;
	DataDef *ltype = regdp.second;
	regdefp_t rhs_rdp = {nullptr, nullptr, nullptr};
	Operand &rval = right->compile(pgm, rhs_rdp);
	DataDef *rtype = rhs_rdp.second ? rhs_rdp.second : ltype;
	pgm.safemul(lval, rval, ltype, rtype);
	return finish_general_binop(pgm, regdp, lval, c);
    }
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenMul::compile() left->compile() cleared datatype!"; }
    DataDef *ltype = regdp.second;
    // Compile RHS with its natural type (not forced to LHS type)
    // so int*double keeps the double in Xmm instead of truncating
    regdefp_t rhs_rdp = {nullptr, nullptr, nullptr};
    Operand &rval = right->compile(pgm, rhs_rdp);
    DataDef *rtype = rhs_rdp.second ? rhs_rdp.second : ltype;
    pgm.safemul(lval, rval, ltype, rtype);
    return finish_general_binop(pgm, regdp, lval, c);
}

// divide two numbers
Operand &TokenDiv::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDiv::Compile() TOP" << endl);
    if ( !left )  { throw "/ missing lval operand"; }
    if ( !right ) { throw "/ missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator/", regdp, _operand) )
	return _operand;
    if ( (left->datadef() && left->datadef()->is_complex())
      || (right->datadef() && right->datadef()->is_complex()) )
	return emit_complex_muldiv(pgm, left, right, /*divide=*/true, regdp, _operand);
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    // Division and modulo must respect operand signedness for correct
    // instruction selection (div vs idiv). If the caller pre-set
    // regdp.second to a signed destination type but the operands'
    // natural type is unsigned, override to the operands' type so
    // safediv emits unsigned division.
    {
	DataDef *natural = infer_numeric_type(left, right);
	if ( should_use_natural_divmod_type(regdp.second, natural) )
	    regdp.second = natural;
    }
    settype(pgm, regdp);				 // set regdp.second type
    {
	bool real_ops = (left && left->is_real()) || (right && right->is_real());
	bool dest_int_real = real_ops && regdp.second && regdp.second->is_integer();
	if ( !dest_int_real && is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	    return emit_plain_divmod(pgm, left, right, regdp.second, /*return_remainder=*/false, regdp, _operand, "_div_l");
    }
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand remainder = regdp.second->newreg(pgm.cc, "remainder");
    Operand &dividend = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenDiv::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "divisor");
    regdp.first = &tmp;
    Operand &divisor = right->compile(pgm, regdp);
    pgm.safexor(remainder, remainder);
    pgm.safediv(remainder, dividend, divisor, regdp.second);
    return finish_general_binop(pgm, regdp, dividend, c);
}

// modulus
Operand &TokenMod::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenMod::Compile() TOP" << endl);
    if ( !left )  { throw "% missing lval operand"; }
    if ( !right ) { throw "% missing rval operand"; }
    if ( can_optimize() )  {return optimize(pgm, regdp);}
    // Override signed destination when operands are unsigned (same as TokenDiv).
    {
	DataDef *natural = infer_numeric_type(left, right);
	if ( should_use_natural_divmod_type(regdp.second, natural) )
	    regdp.second = natural;
    }
    settype(pgm, regdp);
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_divmod(pgm, left, right, regdp.second, /*return_remainder=*/true, regdp, _operand, "_mod_l");

    if ( !regdp.second )
	regdp.second = &ddINT;
    // begin_general_binop allocates the scratch Reg that becomes our
    // remainder — this is the one place the cascade's "scratch" doubles
    // as the op's result, because safediv writes the remainder into it.
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &remainder = *regdp.first;
    Operand dividend = regdp.second->newreg(pgm.cc, "dividend");
    regdp.first = &dividend;
    left->compile(pgm, regdp);
    Operand divisor = regdp.second->newreg(pgm.cc, "divisor");
    regdp.first = &divisor;
    right->compile(pgm, regdp);
    pgm.safexor(remainder, remainder);
    pgm.safediv(remainder, dividend, divisor);
    return finish_general_binop(pgm, regdp, remainder, c);
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

	if ( lval.isMem() )
	{
	    x86::Gp tmp = pgm.cc.newIntPtr("ostream_ptr");
	    pgm.cc.lea(tmp, lval.as<x86::Mem>());
	    lval = tmp;
	}
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
	// only pass ostream as object for functions that consume it (e.g. endl)
	// otherwise the ostream gets injected as a hidden first arg to unrelated functions
	if ( right->type() == TokenType::ttCallFunc
	&&   dynamic_cast<TokenCallFunc *>(right)->returns()->has_ostream() )
	    regdp.object = &lval;
	else
	    regdp.object = NULL;
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
	if ( regdp.second->is_string() )
	{
	    if ( !regdp.first ) { pgm.Throw(this) << "TokenBSL::compile() regdp.first is NULL" << flush; }
	    x86::Gp str_arg = pgm.cc.newIntPtr("__cout_str");
	    lea_var_to_gp(pgm, *regdp.first, str_arg);
	    DBG(cout << "TokenBSL::compile() regdp.second->is_string()" << endl);
	    DBG(pgm.cc.comment("TokenBSL::compile() regdp.second->is_string()"));
	    DBG(pgm.cc.comment("pgm.cc.call(streamout_string)"));
            InvokeNode* call; pgm.cc.invoke(&call, imm(streamout_string), FuncSignature::build<void, void *, void *>());
	    DBG(pgm.cc.comment("call->setArg(0, lval)"));
	    if ( lval.as<BaseReg>().isGroup(RegGroup::kVec) )
		call->setArg(0, lval.as<x86::Xmm>());
	    else
	    if ( lval.as<BaseReg>().isGroup(RegGroup::kGp) )
		call->setArg(0, lval.as<x86::Gp>());
	    else
		throw "TokenBSL::compile() lval unsupported register type";
	    DBG(pgm.cc.comment("call->setArg(1, rval)"));
	    call->setArg(1, str_arg);
	}
	else
	if ( regdp.second->type() == DataType::dtCHARptr )
	{
	    if ( !regdp.first ) { pgm.Throw(this) << "TokenBSL::compile() regdp.first is NULL" << flush; }
	    DBG(cout << "TokenBSL::compile() regdp.second->is_cstr()" << endl);
	    DBG(pgm.cc.comment("TokenBSL::compile() regdp.second->is_cstr()"));
	    // copy cstr pointer to a fresh register to avoid RA conflicts
	    x86::Gp cstr_tmp = pgm.cc.newIntPtr("cstr_out");
	    load_var_to_gp(pgm, *regdp.first, cstr_tmp);
            InvokeNode* call; pgm.cc.invoke(&call, imm(streamout_cstr), FuncSignature::build<void, void *, const char *>());
	    call->setArg(0, lval.as<x86::Gp>());
	    call->setArg(1, cstr_tmp);
	}
	else
	if ( regdp.second->is_numeric() )
	{
	    DBG(cout << "TokenBSL::compile() regdp.second->is_numeric()" << endl);
	    DBG(pgm.cc.comment("pgm.cc.call(streamout_numeric)"));
            InvokeNode *call;
	    switch(regdp.second->type())
	    {
		case DataType::dtCHAR:	pgm.cc.invoke(&call, imm(streamout_numeric<char>), FuncSignature::build<void, void *, char>());	break;
		case DataType::dtBOOL:	pgm.cc.invoke(&call, imm(streamout_numeric<bool>), FuncSignature::build<void, void *, bool>());	break;
		case DataType::dtINT16:	pgm.cc.invoke(&call, imm(streamout_numeric<int16_t>), FuncSignature::build<void, void *, int16_t>());	break;
		case DataType::dtINT24:	pgm.cc.invoke(&call, imm(streamout_numeric<int16_t>), FuncSignature::build<void, void *, int16_t>());	break;
		case DataType::dtINT32:	pgm.cc.invoke(&call, imm(streamout_numeric<int32_t>), FuncSignature::build<void, void *, int32_t>());	break;
		case DataType::dtINT64:	pgm.cc.invoke(&call, imm(streamout_numeric<int64_t>), FuncSignature::build<void, void *, int64_t>());	break;
		case DataType::dtUINT8:	pgm.cc.invoke(&call, imm(streamout_numeric<uint8_t>), FuncSignature::build<void, void *, uint8_t>());	break;
		case DataType::dtUINT16:pgm.cc.invoke(&call, imm(streamout_numeric<uint16_t>), FuncSignature::build<void, void *, uint16_t>());break;
		case DataType::dtUINT24:pgm.cc.invoke(&call, imm(streamout_numeric<uint16_t>), FuncSignature::build<void, void *, uint16_t>());break;
		case DataType::dtUINT32:pgm.cc.invoke(&call, imm(streamout_numeric<uint32_t>), FuncSignature::build<void, void *, uint32_t>());break;
		case DataType::dtUINT64:pgm.cc.invoke(&call, imm(streamout_numeric<uint64_t>), FuncSignature::build<void, void *, uint64_t>());break;
		case DataType::dtFLOAT: DBG(pgm.cc.comment("pgm.cc.call(imm(streamout_numeric<float>),  FuncSignature::build<void, void *, float>())"));
		pgm.cc.invoke(&call, imm(streamout_numeric<float>),  FuncSignature::build<void, void *, float>());	break;
		case DataType::dtDOUBLE: DBG(pgm.cc.comment("pgm.cc.call(imm(streamout_numeric<double>), FuncSignature::build<void, void *, double>())"));
		pgm.cc.invoke(&call, imm(streamout_numeric<double>), FuncSignature::build<void, void *, double>());	break;
		default: throw "TokenBSL::compile() unsupported numeric type";
	    }
	    DBG(pgm.cc.comment("about to setArg(0)"));
	    if ( lval.as<BaseReg>().isGroup(RegGroup::kVec) )
	    {
		DBG(pgm.cc.comment("call->setArg(0, lval.as<x86::Xmm>())"));
		call->setArg(0, lval.as<x86::Xmm>());
	    }
	    else
	    if ( lval.as<BaseReg>().isGroup(RegGroup::kGp) )
	    {
		DBG(pgm.cc.comment("call->setArg(0, lval.as<x86::Gp>())"));
		DBG(cout << "call->setArg(0, lval.as<x86::Gp>()) size=" << lval.x86RmSize() << " regdp.second->size=" << regdp.second->size << " type " << regdp.second->name << endl);
		call->setArg(0, lval.as<x86::Gp>());
	    }
	    else
		throw "TokenBSL::compile() lval unsupported register type";

	    IRBuilder ir(pgm.cc);
	    IRValue stream_arg = ir.load(ir.coerce(ir_from_operand(*regdp.first, regdp.second), regdp.second));
	    if ( stream_arg.op.isReg() )
	    {
		if ( stream_arg.op.as<BaseReg>().isGroup(RegGroup::kVec) )
		{
		    DBG(cout << "call->setArg(1, stream_arg.op.as<x86::Xmm>()) size=" << stream_arg.op.x86RmSize() << " regdp.second->size=" << regdp.second->size << " type " << regdp.second->name << endl);
		    DBG(pgm.cc.comment("call->setArg(1, regdp.first->as<x86::Xmm>())"));
		    call->setArg(1, stream_arg.op.as<x86::Xmm>());
		}
		else
		if ( stream_arg.op.as<BaseReg>().isGroup(RegGroup::kGp) )
		    call->setArg(1, stream_arg.op.as<x86::Gp>());
		else
		    throw "TokenBSL::compile() unexpected parameter Operand";
	    }
	    else
		throw "TokenBSL::compile() unexpected normalized numeric operand";
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

    DataDef *caller_type = regdp.second;
    DataDef *shift_type = promoted_shift_result_type(left);
    if ( !shift_type )
    {
	settype(pgm, regdp);
	shift_type = regdp.second;
    }
    regdp.second = caller_type ? caller_type : shift_type;
    // If either operand is SIMD, override shift_type to the SIMD type.
    DataDef *simd_type = (left && left->datadef() && left->datadef()->is_simd()) ? left->datadef()
		       : (right && right->datadef() && right->datadef()->is_simd()) ? right->datadef()
		       : nullptr;
    if ( simd_type )
	shift_type = simd_type;
    if ( shift_type && shift_type->is_simd() )
	return emit_large_simd_bitwise(pgm, left, right, SimdBitKind::Shl,
	    static_cast<DataDefSIMD *>(shift_type), regdp, _operand);
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && shift_type && shift_type->is_integer() )
	return emit_plain_bitop2(pgm, left, right, shift_type, &Program::safeshl,
				 /*right_must_be_gp=*/true,
				 /*left_precision_only=*/true,
				 regdp, _operand, "_shl_l");
    regdp.second = shift_type;
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBSL::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeshl(lval, rval, regdp.second);
    return finish_general_binop(pgm, regdp, lval, c);
}

// bit shift right / stream input (>>)
Operand &TokenBSR::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBSR::Compile() TOP" << endl);
    if ( !left )  { throw ">> missing lval operand"; }
    if ( !right ) { throw ">> missing rval operand"; }

    // istream input: cin >> var
    if ( left->type() == TokenType::ttVariable && dynamic_cast<TokenVar *>(left)->var.type->has_istream() )
    {
	TokenVar *tvl = dynamic_cast<TokenVar *>(left);
	DBG(pgm.cc.comment("TokenBSR::compile() istream >>"));
	Operand &lval = tvl->operand(pgm); // get istream register

	if ( lval.isMem() )
	{
	    x86::Gp tmp = pgm.cc.newIntPtr("istream_ptr");
	    pgm.cc.lea(tmp, lval.as<x86::Mem>());
	    lval = tmp;
	}
	if ( !lval.isReg() )
	    throw "TokenBSR::compile() lval operand not a register";

	// converge chained >>: cin >> a >> b
	if ( right->id() == TokenID::tkBSR && !right->is_bracketed() )
	{
	    DBG(cout << "TokenBSR::compile() converging right BSR(>>) to left istream" << endl);
	    TokenBSR tmpsin;
	    TokenBSR *rsin = static_cast<TokenBSR *>(right);
	    tmpsin.left = left;
	    tmpsin.right = rsin->left;
	    tmpsin.compile(pgm, regdp);
	    tmpsin.right = rsin->right;
	    tmpsin.compile(pgm, regdp);
	    regdp.first = &lval;
	    regdp.second = tvl->var.type;
	    return *regdp.first;
	}

	// compile right side to get the target variable
	regdp.first = NULL;
	regdp.second = NULL;
	regdp.object = &lval;
	right->compile(pgm, regdp);

	Operand *input_dest = NULL;
	if ( TokenVar *tvr = dynamic_cast<TokenVar *>(right) )
	    input_dest = &tvr->operand(pgm);
	else if ( TokenMember *tmr = dynamic_cast<TokenMember *>(right) )
	    input_dest = &tmr->operand(pgm);
	Operand *store_dest = input_dest ? input_dest : regdp.first;

	if ( !regdp.second )
	    throw "TokenBSR::compile() unable to determine rval type for >>";
	if ( !store_dest )
	    throw "TokenBSR::compile() unable to determine input destination for >>";

	InvokeNode *call;
	if ( regdp.second->is_string() )
	{
	    DBG(pgm.cc.comment("streamin_string"));
	    x86::Gp str_dst = pgm.cc.newIntPtr("__cin_str");
	    lea_var_to_gp(pgm, *store_dest, str_dst);
	    pgm.cc.invoke(&call, imm(streamin_string), FuncSignature::build<void *, void *, void *>());
	    call->setArg(0, lval.as<x86::Gp>());
	    call->setArg(1, str_dst);
	}
	else if ( regdp.second->is_integer() )
	{
	    // streamin_int expects a pointer to int64_t — use a temp stack slot
	    DBG(pgm.cc.comment("streamin_int"));
	    x86::Mem tmp_slot = pgm.cc.newStack(8, 8);
	    x86::Gp tmp_addr = pgm.cc.newIntPtr("__cin_addr");
	    pgm.cc.lea(tmp_addr, tmp_slot);
	    pgm.cc.invoke(&call, imm(streamin_int), FuncSignature::build<void *, void *, void *>());
	    call->setArg(0, lval.as<x86::Gp>());
	    call->setArg(1, tmp_addr);
	    Operand store_tmp;
	    regdefp_t store_rdp = {store_dest, regdp.second, NULL};
	    emit_ir_value(pgm, IRValue::mem(tmp_slot, &ddINT64), store_rdp, store_tmp, regdp.second);
	}
	else if ( regdp.second->is_real() )
	{
	    DBG(pgm.cc.comment("streamin_double"));
	    x86::Mem tmp_slot = pgm.cc.newStack(8, 8);
	    x86::Gp tmp_addr = pgm.cc.newIntPtr("__cin_addr");
	    pgm.cc.lea(tmp_addr, tmp_slot);
	    pgm.cc.invoke(&call, imm(streamin_double), FuncSignature::build<void *, void *, void *>());
	    call->setArg(0, lval.as<x86::Gp>());
	    call->setArg(1, tmp_addr);
	    Operand store_tmp;
	    regdefp_t store_rdp = {store_dest, regdp.second, NULL};
	    emit_ir_value(pgm, IRValue::mem(tmp_slot, &ddDOUBLE), store_rdp, store_tmp, regdp.second);
	}
	else
	    throw "TokenBSR::compile() unsupported type for >> input";

	regdp.first = &lval;
	regdp.second = tvl->var.type;
	return *regdp.first;
    }

    // bitwise right-shift
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    DataDef *caller_type = regdp.second;
    DataDef *shift_type = promoted_shift_result_type(left);
    if ( !shift_type )
    {
	settype(pgm, regdp);
	shift_type = regdp.second;
    }
    regdp.second = caller_type ? caller_type : shift_type;
    {
	DataDef *simd_type = (left && left->datadef() && left->datadef()->is_simd()) ? left->datadef()
			   : (right && right->datadef() && right->datadef()->is_simd()) ? right->datadef()
			   : nullptr;
	if ( simd_type )
	    shift_type = simd_type;
    }
    if ( shift_type && shift_type->is_simd() )
	return emit_large_simd_bitwise(pgm, left, right, SimdBitKind::Shr,
	    static_cast<DataDefSIMD *>(shift_type), regdp, _operand);
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && shift_type && shift_type->is_integer() )
	return emit_plain_bitop2(pgm, left, right, shift_type, &Program::safeshr,
				 /*right_must_be_gp=*/true,
				 /*left_precision_only=*/true,
				 regdp, _operand, "_shr_l");
    regdp.second = shift_type;
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBSR::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeshr(lval, rval, regdp.second);
    return finish_general_binop(pgm, regdp, lval, c);
}

// bitwise or |
Operand &TokenBor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( regdp.second && regdp.second->is_simd() )
	return emit_large_simd_bitwise(pgm, left, right, SimdBitKind::Or,
	    static_cast<DataDefSIMD *>(regdp.second), regdp, _operand);
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_bitop2(pgm, left, right, regdp.second, &Program::safeor,
				 /*right_must_be_gp=*/true,
				 /*left_precision_only=*/false,
				 regdp, _operand, "_or_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBor::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeor(lval, rval, regdp.second);
    return finish_general_binop(pgm, regdp, lval, c);
}

// bitwise xor ^
Operand &TokenXor::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenXor::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( regdp.second && regdp.second->is_simd() )
	return emit_large_simd_bitwise(pgm, left, right, SimdBitKind::Xor,
	    static_cast<DataDefSIMD *>(regdp.second), regdp, _operand);
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_bitop2(pgm, left, right, regdp.second, &Program::safexor,
				 /*right_must_be_gp=*/true,
				 /*left_precision_only=*/false,
				 regdp, _operand, "_xor_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenXor::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safexor(lval, rval, regdp.second);
    return finish_general_binop(pgm, regdp, lval, c);
}

// bitwise and &
Operand &TokenBand::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBand::Compile() TOP" << endl);
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( regdp.second && regdp.second->is_simd() )
	return emit_large_simd_bitwise(pgm, left, right, SimdBitKind::And,
	    static_cast<DataDefSIMD *>(regdp.second), regdp, _operand);
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second )
    {
	// Bitwise AND always produces an integer. When the caller wants a
	// real destination (e.g. `double d = i & 7`), compute the AND in
	// the operands' integer type, then coerce int→double on return.
	DataDef *comp_type = regdp.second;
	if ( comp_type->is_real() && !comp_type->is_simd() )
	{
	    DataDef *natural = infer_numeric_type(left, right);
	    comp_type = (natural && natural->is_integer()) ? natural : &ddINT64;
	    Operand *caller_dest = regdp.first;
	    DataDef *caller_type = regdp.second;
	    regdp.first = NULL;
	    regdp.second = comp_type;
	    Operand &int_result = emit_plain_bitop2(pgm, left, right, comp_type,
		&Program::safeand, true, false, regdp, _operand, "_and_l");
	    regdp.first = caller_dest;
	    regdp.second = caller_type;
	    return emit_ir_value(pgm, IRValue::reg(int_result, comp_type),
		regdp, _operand, caller_type);
	}
	if ( comp_type->is_integer() )
	    return emit_plain_bitop2(pgm, left, right, comp_type, &Program::safeand,
				     true, false, regdp, _operand, "_and_l");
    }
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBand::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeand(lval, rval, regdp.second);
    return finish_general_binop(pgm, regdp, lval, c);
}

// bitwise not ~
Operand &TokenBnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBnot::Compile() TOP" << endl);
    if ( left )   { throw "Bitwise not has lval!"; }
    if ( !right ) { throw "~ missing rval operand"; }
    if ( right->datadef() && right->datadef()->is_complex() )
	return emit_complex_conjugate_expr(pgm, right, right->datadef(), regdp, _operand);
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( regdp.second && regdp.second->is_simd() )
	return emit_simd_bitwise_not(pgm, right, static_cast<DataDefSIMD *>(regdp.second),
	    regdp, _operand);
    if ( right && right->datadef() && right->datadef()->is_integer() )
    {
	Operand *caller_dest = regdp.first;
	DataDef *requested_type = regdp.second;
	Operand out_norm;
	DataDef *rtype = token_numeric_type(right);
	if ( !rtype )
	    rtype = right->datadef() ? right->datadef() : regdp.second;
	DataDef *result_type = rtype && rtype->is_integer()
	    ? promote_c_integer_type(rtype) : regdp.second;
	DBG(std::cout << "TokenBnot: integer path, rtype=" << (rtype ? rtype->name : "null") << " size=" << (rtype ? (int)rtype->size : -1) << std::endl);
	Operand &rval = compile_token_gp_normalized(pgm, right, result_type, out_norm);
	pgm.safenot(rval);
	// Narrow result to semantic type width for sub-64-bit types.
	// ~0U must produce a 32-bit 0xFFFFFFFF, not 64-bit 0xFFFFFFFFFFFFFFFF.
	// Using mov r32,r32 to truncate (implicit zero-extend to 64-bit) so
	// subsequent comparisons use the right register width.
	if ( result_type && result_type->size == 4 && rval.isReg()
	  && rval.as<x86::Gp>().isGpq() )
	{
	    DBG(std::cout << "TokenBnot: narrowing to Gpd" << std::endl);
	    x86::Gp narrow = pgm.cc.newGpd("~narrow");
	    pgm.cc.mov(narrow, rval.as<x86::Gp>().r32());
	    rval = narrow;
	}
	else if ( result_type && result_type->size < 4 && rval.isReg() )
	    pgm.cc.and_(rval.as<x86::Gp>(), (int64_t)((1ULL << (result_type->size * 8)) - 1));
	_operand = rval;
	regdp.second = result_type;
	regdp.first = &_operand;
	if ( caller_dest
	  && (caller_dest->isMem() || !requested_type || requested_type == result_type) )
	{
	    regdp.first = caller_dest;
	    return emit_ir_value(pgm, IRValue::reg(rval, regdp.second), regdp, _operand, regdp.second);
	}
	return _operand;
    }
    if ( !regdp.first )					 // if not passed a register:
    {
	_operand = regdp.second->newreg(pgm.cc, "_reg"); // use internal operand
	regdp.first = &_operand;			 // pass _operand along
    }
    Operand &rval = right->compile(pgm, regdp);		 // compile right side ref=rval
    if ( !regdp.second ) { throw "TokenBnot::compile() right->compile cleared datatype"; }
    pgm.safenot(rval);					 // type safe bitwise not
    // Narrow result to semantic type width for sub-64-bit types.
    if ( regdp.second && regdp.second->size == 4 && rval.isReg()
      && rval.as<x86::Gp>().isGpq() )
    {
	x86::Gp narrow = pgm.cc.newGpd("~narrow");
	pgm.cc.mov(narrow, rval.as<x86::Gp>().r32());
	_operand = narrow;
	rval = _operand;
    }
    else if ( regdp.second && regdp.second->size < 4 && rval.isReg() )
	pgm.cc.and_(rval.as<x86::Gp>(), (int64_t)((1ULL << (regdp.second->size * 8)) - 1));
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
    Operand *caller_dest = regdp.first;
    DataDef *test_type = infer_numeric_type(nullptr, right);
    Operand test_storage;
    Operand &rval = compile_token_normalized(pgm, right, test_type, nullptr, test_storage);
    DBG(cout << "TokenLnot::compile() pgm.safetest(rval, rval)" << endl);
    DBG(pgm.cc.comment("TokenLnot::compile() pgm.safetest(rval, rval)"));
    pgm.testzero(rval);					 // test rval is 0
    x86::Gp result = pgm.cc.newGpq("_lnot");
    DBG(pgm.cc.comment("TokenLnot::compile() pgm.cc.sete(result)"));
    pgm.safesete(result);
    regdp.second = &ddINT64;
    _operand = result;
    regdp.first = &_operand;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(result, &ddINT64), regdp, _operand, &ddINT64);
    }
    return _operand;
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
    Operand *caller_dest = regdp.first;
    DataDef *test_type = infer_numeric_type(left, right);
    Operand left_storage;
    Operand right_storage;
    // Short-circuit: compile+test left, branch out if non-zero BEFORE
    // compiling right. Otherwise `p || p->next` evaluates p->next even
    // when p is non-NULL, defeating the C short-circuit guarantee.
    Operand &lval = compile_token_normalized(pgm, left, test_type, nullptr, left_storage);
    x86::Gp result = pgm.cc.newGpq("_lor");
    pgm.testzero(lval);					 // test lval is 0
    pgm.safesetne(result);				 // result = (lval != 0) ? 1 : 0
    pgm.cc.jne(done);					 // if lval != 0, skip rval
    Operand &rval = compile_token_normalized(pgm, right, test_type, nullptr, right_storage);
    pgm.testzero(rval);					 // test rval is 0
    pgm.safesetne(result);				 // if rval !=0, result = 1
    pgm.cc.bind(done);					 // done is here
    regdp.second = &ddINT64;
    _operand = result;
    regdp.first = &_operand;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(result, &ddINT64), regdp, _operand, &ddINT64);
    }
    return _operand;
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
    Operand *caller_dest = regdp.first;
    DataDef *test_type = infer_numeric_type(left, right);
    Operand left_storage;
    Operand right_storage;
    // Short-circuit: compile + test left, branch if zero BEFORE compiling
    // right. Otherwise `p && p->next` evaluates p->next when p is NULL
    // and segfaults — every C/C++ programmer relies on the short-circuit.
    Operand &lval = compile_token_normalized(pgm, left, test_type, nullptr, left_storage);
    x86::Gp result = pgm.cc.newGpq("_land");
    pgm.safexor(result, result);			 // result = 0
    pgm.testzero(lval);					 // test lval is 0
    pgm.cc.je(done);					 // if lval == 0, result stays 0
    Operand &rval = compile_token_normalized(pgm, right, test_type, nullptr, right_storage);
    pgm.testzero(rval);					 // test rval is 0
    pgm.safesetne(result);				 // if rval !=0, result = 1
    pgm.cc.bind(done);					 // done is here
    regdp.second = &ddINT64;
    _operand = result;
    regdp.first = &_operand;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(result, &ddINT64), regdp, _operand, &ddINT64);
    }
    return _operand;
}

// Equal to: ==
Operand &TokenEquals::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "== missing lval operand"; }
    if ( !right ) { throw "== missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator==", regdp, _operand) )
	return _operand;
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Eq, regdp, _operand);
}

// Not equal to: !=
Operand &TokenNotEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator!=", regdp, _operand) )
	return _operand;
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Ne, regdp, _operand);
}

// Less than: <
Operand &TokenLT::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "< missing lval operand"; }
    if ( !right ) { throw "< missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator<", regdp, _operand) )
	return _operand;
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Lt, regdp, _operand);
}

// Less than or equal to: <=
Operand &TokenLE::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "<= missing lval operand"; }
    if ( !right ) { throw "<= missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator<=", regdp, _operand) )
	return _operand;
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Le, regdp, _operand);
}

// Greater than: >
Operand &TokenGT::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "> missing lval operand"; }
    if ( !right ) { throw "> missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator>", regdp, _operand) )
	return _operand;
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Gt, regdp, _operand);
}

// Greater than or equal to: >=
Operand &TokenGE::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw ">= missing lval operand"; }
    if ( !right ) { throw ">= missing rval operand"; }
    if ( try_class_operator_dispatch(pgm, left, right, "operator>=", regdp, _operand) )
	return _operand;
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Ge, regdp, _operand);
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
    Operand *caller_dest = regdp.first;
    DataDef *cmp_type = infer_numeric_type(left, right);
    Operand left_norm;
    Operand right_norm;
    Operand &lval = compile_token_normalized(pgm, left, cmp_type, nullptr, left_norm);
    Operand &rval = compile_token_normalized(pgm, right, cmp_type, nullptr, right_norm);
    x86::Gp result = pgm.cc.newGpq("_cmp_3way");
    DBG(pgm.cc.comment("Token3Way::compile() pgm.safecmp(lval, rval)"));
    pgm.safecmp(lval, rval, cmp_type);			 // typesafe comparison

    pgm.safesetg(result);					 // set result to 1 if >
    pgm.cc.jg(done);					 // if >, jump to done
    pgm.safesetl(result);					 // set result to 1 if <
    pgm.cc.jl(sign);					 // if <, jump to negate
    pgm.safexor(result, result);				 // result = 0
    pgm.cc.bind(sign);
    pgm.safeneg(result);					 // result ? 1 : -1
    pgm.cc.bind(done);					 // done
    regdp.second = &ddINT64;
    _operand = result;
    regdp.first = &_operand;
    if ( caller_dest )
    {
	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::reg(result, &ddINT64), regdp, _operand, &ddINT64);
    }
    return _operand;
}

DataDef *TokenComplexPart::datadef() const
{
    DataDef *expr_dd = expr ? expr->datadef() : NULL;
    if ( expr_dd && expr_dd->is_complex() )
	return complex_component_type(expr_dd);
    if ( imag_part )
	return expr_dd && expr_dd->is_real() ? expr_dd : &ddINT64;
    return expr_dd ? expr_dd : &ddINT64;
}

Operand &TokenComplexPart::operand(Program &pgm)
{
    DataDef *expr_dd = expr ? expr->datadef() : NULL;
    if ( !expr_dd || !expr_dd->is_complex() )
	throw "TokenComplexPart::operand() requires a complex lvalue";
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(expr_dd);
    if ( !cdd )
	throw "TokenComplexPart::operand() expected complex datadef";
    regdefp_t sub = {nullptr, expr_dd, nullptr};
    Operand &base_op = expr->compile(pgm, sub);
    size_t ofs = cdd->component_offset(imag_part);
    DataDef *part_dd = datadef();

    if ( base_op.isMem() )
    {
	_operand = base_op.as<x86::Mem>();
	_operand.as<x86::Mem>().addOffset((int64_t)ofs);
	_operand.as<x86::Mem>().setSize((uint32_t)part_dd->size);
	return _operand;
    }

    x86::Gp base_gp;
    if ( base_op.isImm() )
    {
	base_gp = pgm.cc.newIntPtr(imag_part ? "__imag_imm" : "__real_imm");
	pgm.cc.mov(base_gp, base_op.as<Imm>());
    }
    else
	base_gp = base_op.as<x86::Gp>();

    _operand = x86::ptr(base_gp, (int32_t)ofs, (uint32_t)part_dd->size);
    return _operand;
}

Operand &TokenComplexPart::compile(Program &pgm, regdefp_t &regdp)
{
    DataDef *expr_dd = expr ? expr->datadef() : NULL;
    DataDef *part_dd = datadef();

    if ( expr_dd && expr_dd->is_complex() )
    {
	Operand &part_mem = operand(pgm);
	return emit_ir_value(pgm, IRValue::mem(part_mem.as<x86::Mem>(), part_dd),
			     regdp, _operand, part_dd);
    }

    if ( imag_part )
    {
	x86::Gp zero = pgm.cc.newGpq("__imag_zero");
	pgm.cc.xor_(zero, zero);
	_operand = zero;
	regdp.first = &_operand;
	regdp.second = part_dd;
	return _operand;
    }

    regdefp_t inner = {regdp.first, regdp.second, regdp.object};
    Operand &result = expr->compile(pgm, inner);
    regdp.first = &result;
    regdp.second = part_dd;
    return result;
}

Operand &TokenDerefStep::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment(increment ? "TokenDerefStep::compile(*ptr++)" : "TokenDerefStep::compile(*ptr--)"));
    if ( !regdp.second )
	regdp.second = deref_type;

    Operand &ptr_op = pgm.tkFunction->voperand(pgm, &var);
    if ( !ptr_op.isReg() || !ptr_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "TokenDerefStep::compile() pointer operand is not a gp register";

    x86::Gp ptr_reg = ptr_op.as<x86::Gp>();
    x86::Gp old_ptr = pgm.cc.newIntPtr(increment ? "deref_postinc_ptr" : "deref_postdec_ptr");
    pgm.safemov(old_ptr, ptr_reg);

    int step = deref_type ? (int)deref_type->size : 1;
    if ( step <= 0 )
	step = 1;
    if ( increment )
	pgm.safeadd(ptr_reg, step, var.type, deref_type);
    else
	pgm.safesub(ptr_reg, step, var.type, deref_type);
    var.modified();

    if ( deref_type->is_numeric() || deref_type->is_pointer() )
    {
	x86::Mem mem = x86::ptr(old_ptr, 0, (uint32_t)deref_type->size);
	return emit_ir_value(pgm, IRValue::mem(mem, deref_type), regdp, _operand, deref_type);
    }

    _operand = old_ptr;
    regdp.first = &_operand;
    regdp.second = deref_type;
    return _operand;
}

// (TYPE *) cast expression
Operand &TokenCast::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenCast::compile()"));
    // `(void)expr` discards the value; it should not force the inner
    // expression through a nonexistent "coerce to void" path.
    // `(void *)expr` is a real reinterpreting cast — DataDef::rawtype()
    // strips the pointer ref bias and reports dtVOID for both, so the
    // is_pointer() guard keeps `(void *)` flowing through the normal
    // cast path that sets regdp.second = cast_type.
    if ( cast_type && cast_type->rawtype() == DataType::dtVOID
      && !cast_type->is_pointer() )
    {
	regdefp_t inner_rdp = {NULL, expr ? expr->datadef() : NULL, NULL};
	Operand &result = expr->compile(pgm, inner_rdp);
	regdp.second = cast_type;
	if ( !regdp.first )
	    regdp.first = &result;
	return result;
    }
    // SIMD-to-SIMD reinterpret cast (same size, >8 bytes): e.g.
    // (V8)(V64)x or (V64)(V8)x where both are 32-byte vectors.
    // All bits are preserved — just change the type label.  For >16-byte
    // (Mem-backed) vectors, emit a raw aggregate copy.
    {
	DataDef *src_type_early = expr ? expr->datadef() : NULL;
	if ( cast_type && cast_type->is_simd() && cast_type->size > 8
	  && src_type_early && src_type_early->is_simd()
	  && src_type_early->size == cast_type->size )
	{
	    regdefp_t inner_rdp = {NULL, src_type_early, NULL};
	    Operand &src_op = expr->compile(pgm, inner_rdp);
	    DataDef *actual_src = inner_rdp.second ? inner_rdp.second : src_type_early;
	    regdp.second = cast_type;
	    if ( cast_type->size > 16 )
	    {
		// Mem-backed wide SIMD: copy via aggregate copy
		x86::Mem buf = pgm.cc.newStack((uint32_t)cast_type->size, 16);
		Operand dst = buf;
		emit_raw_aggregate_copy(pgm, dst, src_op, actual_src, "cast_simd_reinterp");
		if ( regdp.first && regdp.first->isMem() )
		{
		    Operand caller_dst = *regdp.first;
		    emit_raw_aggregate_copy(pgm, caller_dst, dst, cast_type, "cast_simd_reinterp_dst");
		    return *regdp.first;
		}
		_operand = buf;
		regdp.first = &_operand;
		return _operand;
	    }
	    // 16-byte SIMD: use Xmm path
	    x86::Xmm out = cast_type->newreg(pgm.cc, "cast_simd_reinterp").as<x86::Xmm>();
	    if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.safemov(out, src_op.as<x86::Xmm>(), cast_type, actual_src);
	    else if ( src_op.isMem() )
		load_mem_to_xmm(pgm, out, src_op.as<x86::Mem>(), actual_src);
	    else if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.movq(out, src_op.as<x86::Gp>());
	    else
		throw "TokenCast SIMD reinterpret expects Xmm/Mem/Gp source";
	    if ( regdp.first )
	    {
		pgm.safemov(*regdp.first, *(Operand *)&out, cast_type, cast_type);
		return *regdp.first;
	    }
	    _operand = out;
	    regdp.first = &_operand;
	    return _operand;
	}
    }

    // Casting a std::string expression to `char *` needs an actual
    // conversion via string_cstr — a bare reinterpretation would leave
    // the caller holding the std::string object's address, not its
    // c_str() data. Detect the case before compiling so we can route
    // the inner expression into a fresh register and run string_cstr
    // on it.
    bool str_to_cstr = expr && expr->datadef()
	&& expr->datadef()->rawtype() == DataType::dtSTRING
	&& cast_type && cast_type->is_pointer()
	&& (cast_type->rawtype() == DataType::dtCHAR
	 || cast_type->rawtype() == DataType::dtUINT8);
    bool scalar_to_complex = expr && expr->datadef()
	&& !expr->datadef()->is_complex()
	&& cast_type && cast_type->is_complex();
    bool scalar_to_union = expr && expr->datadef()
	&& cast_type && cast_type->basetype() == BaseType::btStruct
	&& !expr->datadef()->is_complex()
	&& (expr->datadef()->is_numeric() || expr->datadef()->is_pointer());
    bool complex_to_complex = expr && expr->datadef()
	&& expr->datadef()->is_complex()
	&& cast_type && cast_type->is_complex();
    bool complex_to_scalar = expr && expr->datadef()
	&& expr->datadef()->is_complex()
	&& !effective_pointer_type_for_arith(pgm, expr)
	&& cast_type && (cast_type->is_numeric() || cast_type->is_pointer());
    if ( complex_to_complex )
	return emit_complex_value_from_expr(pgm, expr, cast_type, regdp, _operand);
    if ( scalar_to_union )
    {
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(cast_type);
	if ( sdd && sdd->union_layout )
	    return emit_union_from_scalar(pgm, expr, sdd, regdp, _operand);
    }
    if ( scalar_to_complex )
    {
	regdefp_t inner_rdp = {NULL, expr->datadef(), NULL};
	Operand &scalar_op = expr->compile(pgm, inner_rdp);
	DataDef *actual_src = inner_rdp.second ? inner_rdp.second : expr->datadef();
	return emit_complex_from_scalar(pgm, scalar_op, actual_src, cast_type,
					/*imag_value=*/false, regdp, _operand,
					"__cast_complex");
    }
    if ( complex_to_scalar )
    {
	TokenComplexPart real_part(expr, false);
	regdefp_t inner_rdp = {NULL, complex_component_type(expr->datadef()), NULL};
	Operand &real_op = real_part.compile(pgm, inner_rdp);
	DataDef *real_type = inner_rdp.second ? inner_rdp.second : real_part.datadef();
	regdp.second = cast_type;
	if ( cast_type->is_pointer() && real_type && real_type->is_pointer() )
	{
	    if ( regdp.first )
	    {
		pgm.safemov(*regdp.first, real_op, cast_type, real_type);
		return *regdp.first;
	    }
	    _operand = real_op;
	    regdp.first = &_operand;
	    return _operand;
	}
	IRBuilder ir(pgm.cc);
	IRValue out = ir.coerce(ir_from_operand(real_op, real_type), cast_type);
	out = ir.load(out);
	if ( regdp.first )
	{
	    pgm.safemov(*regdp.first, out.op, cast_type, cast_type);
	    return *regdp.first;
	}
	_operand = out.op;
	regdp.first = &_operand;
	return _operand;
    }
    if ( str_to_cstr )
    {
	Operand cstr_storage;
	Operand &cstr = compile_token_normalized(pgm, expr, cast_type, nullptr, cstr_storage);
	regdp.second = cast_type;
	if ( regdp.first )
	{
	    pgm.safemov(*regdp.first, cstr, cast_type, cast_type);
	    return *regdp.first;
	}
	_operand = cstr;
	regdp.first = &_operand;
	return _operand;
    }
    DataDef *src_ptr_type = effective_pointer_type_for_arith(pgm, expr);
    if ( src_ptr_type && cast_type && (cast_type->is_pointer() || cast_type->is_integer()) )
    {
	regdefp_t inner_rdp = {NULL, src_ptr_type, NULL};
	Operand &src_op = expr->compile(pgm, inner_rdp);
	DataDef *actual_inner = inner_rdp.second ? inner_rdp.second : src_ptr_type;
	regdp.second = cast_type;
	if ( cast_type->is_integer() )
	{
	    IRBuilder ir(pgm.cc);
	    IRValue out = ir.coerce(ir_from_operand(src_op, &ddUINT64), cast_type);
	    out = ir.load(out);
	    if ( regdp.first )
	    {
		pgm.safemov(*regdp.first, out.op, cast_type, cast_type);
		return *regdp.first;
	    }
	    _operand = out.op;
	    regdp.first = &_operand;
	    return _operand;
	}
	if ( regdp.first )
	{
	    pgm.safemov(*regdp.first, src_op, cast_type, actual_inner);
	    return *regdp.first;
	}
	_operand = src_op;
	regdp.first = &_operand;
	return _operand;
    }
    // Real ↔ real cast: `(double)flt_expr` / `(float)dbl_expr` need a
    // cvtss2sd / cvtsd2ss. A bare reinterpret leaves the Xmm holding
    // raw bits in the source precision, so subsequent variadic-arg
    // handling (printf(%f) reads it as double) gets garbage.
    DataDef *src_type = expr ? expr->datadef() : NULL;
    bool src_is_real = src_type && src_type->is_real();
    bool dst_is_real = cast_type && cast_type->is_real();
    if ( cast_type && cast_type->is_simd()
      && src_type && !src_type->is_simd()
      && (src_type->is_integer() || src_type->is_pointer()) )
    {
	regdefp_t inner_rdp = {NULL, NULL, NULL};
	Operand &src_op = expr->compile(pgm, inner_rdp);
	DataDef *actual_src = inner_rdp.second ? inner_rdp.second : src_type;
	IRBuilder ir(pgm.cc);
	IRValue scalar = ir.load(ir_from_operand(src_op, actual_src));
	x86::Xmm out = cast_type->newreg(pgm.cc, "cast_s2v").as<x86::Xmm>();
	if ( scalar.isReg() && scalar.op.as<BaseReg>().isGroup(RegGroup::kVec) )
	    pgm.cc.movq(out, scalar.op.as<x86::Xmm>());
	else
	{
	    if ( !scalar.isReg() || !scalar.op.as<BaseReg>().isGroup(RegGroup::kGp) )
		throw "TokenCast::compile() scalar->SIMD cast expects scalar register";
	    x86::Gp gp = scalar.op.as<x86::Gp>();
	    if ( actual_src && actual_src->size <= 4 )
		pgm.cc.movd(out, gp.r32());
	    else
	    {
		if ( !gp.isGpq() )
		{
		    x86::Gp wide = pgm.cc.newGpq("cast_s2v_wide");
		    pgm.cc.mov(wide.r32(), gp.r32());
		    gp = wide;
		}
		pgm.cc.movq(out, gp.r64());
	    }
	}
	regdp.second = cast_type;
	if ( regdp.first )
	{
	    if ( regdp.first->isReg()
	      && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec) )
	    {
		pgm.safemov(*regdp.first, out, cast_type, cast_type);
		return *regdp.first;
	    }
	    if ( regdp.first->isMem() )
	    {
		x86::Mem dst = regdp.first->as<x86::Mem>();
		store_xmm_to_mem(pgm, dst, out, cast_type);
		return *regdp.first;
	    }
	}
	_operand = out;
	regdp.first = &_operand;
	return _operand;
    }
    if ( src_is_real && dst_is_real && src_type->size != cast_type->size )
    {
	regdefp_t inner_rdp = {NULL, NULL, NULL};
	inner_rdp.second = src_type;
	Operand &src_op = expr->compile(pgm, inner_rdp);
	x86::Xmm out = newScalarXmm(pgm, cast_type, "cast_real");
	if ( cast_type->size == sizeof(float) )
	{
	    // dbl -> flt
	    if ( src_op.isReg() )
		pgm.cc.cvtsd2ss(out, src_op.as<x86::Xmm>());
	    else
		pgm.cc.cvtsd2ss(out, src_op.as<x86::Mem>());
	}
	else
	{
	    // flt -> dbl
	    if ( src_op.isReg() )
		pgm.cc.cvtss2sd(out, src_op.as<x86::Xmm>());
	    else
		pgm.cc.cvtss2sd(out, src_op.as<x86::Mem>());
	}
	regdp.second = cast_type;
	if ( regdp.first && regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    if ( cast_type->size == sizeof(float) )
		pgm.cc.movss(regdp.first->as<x86::Xmm>(), out);
	    else
		pgm.cc.movsd(regdp.first->as<x86::Xmm>(), out);
	    return *regdp.first;
	}
	if ( regdp.first && regdp.first->isMem() )
	{
	    IRBuilder ir(pgm.cc);
	    ir.store(IRValue::mem(regdp.first->as<x86::Mem>(), cast_type), ir_from_operand(out, cast_type));
	    return *regdp.first;
	}
	_operand = out;
	regdp.first = &_operand;
	return _operand;
    }

    // Real → integer cast: compile inner as its natural float/double type,
    // then convert to Gp via cvttss2si / cvttsd2si.  The fallthrough
    // "reinterpret" path below is wrong here: it passes regdp.second =
    // int to the inner compile, which may write to a caller-supplied Xmm
    // destination and then compile_token_normalized double-converts the
    // result (cvtsi2ss on an Xmm register misinterpreted as Gp).
    if ( src_is_real && cast_type && cast_type->is_integer() )
    {
	// SIMD-to-SIMD reinterpret cast (e.g. (__m128i)(__m128d)v):
	// both sides are same-size vectors >8 bytes — preserve all bits.
	// 8-byte SIMD fits in a Gp via movq, so the fallthrough path is
	// correct for those; only >8-byte needs the full-width Xmm path.
	if ( src_type && src_type->is_simd() && cast_type->is_simd()
	  && src_type->size == cast_type->size && src_type->size > 8 )
	{
	    regdefp_t inner_rdp = {NULL, src_type, NULL};
	    Operand &src_op = expr->compile(pgm, inner_rdp);
	    DataDef *actual_src = inner_rdp.second ? inner_rdp.second : src_type;
	    x86::Xmm out = cast_type->newreg(pgm.cc, "cast_simd_r2i").as<x86::Xmm>();
	    if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.safemov(out, src_op.as<x86::Xmm>(), cast_type, actual_src);
	    else if ( src_op.isMem() )
		load_mem_to_xmm(pgm, out, src_op.as<x86::Mem>(), actual_src);
	    else if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.movq(out, src_op.as<x86::Gp>());
	    else
		throw "TokenCast::compile() SIMD real->int reinterpret expects Xmm/Mem/Gp";
	    regdp.second = cast_type;
	    if ( regdp.first )
	    {
		pgm.safemov(*regdp.first, *(Operand *)&out, cast_type, cast_type);
		return *regdp.first;
	    }
	    _operand = out;
	    regdp.first = &_operand;
	    return _operand;
	}

	regdefp_t inner_rdp = {NULL, src_type, NULL};
	Operand &src_op = expr->compile(pgm, inner_rdp);
	bool preserve_simd_bits = src_type->is_simd() || cast_type->is_simd();
	x86::Gp out = preserve_simd_bits
	    ? pgm.cc.newGpq("cast_r2i")
	    : cast_type->newreg(pgm.cc, "cast_r2i").as<x86::Gp>();
	bool unsigned_u32 = cast_type->is_unsigned() && cast_type->size == 4;
	if ( src_type->is_simd() )
	{
	    if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.cc.movq(out, src_op.as<x86::Xmm>());
	    else if ( src_op.isMem() )
		pgm.cc.mov(out, src_op.as<x86::Mem>());
	    else if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.mov(out, src_op.as<x86::Gp>());
	    else
		throw "TokenCast::compile() SIMD real->int expects Xmm/Mem/Gp source";
	}
	else
	if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    if ( unsigned_u32 )
	    {
		Label direct_convert = pgm.cc.newLabel();
		Label convert_done = pgm.cc.newLabel();
		if ( src_type->size == sizeof(float) )
		{
		    x86::Mem limit = pgm.cc.newFloatConst(ConstPoolScope::kLocal, 2147483648.0f);
		    limit.setSize(sizeof(float));
		    pgm.cc.ucomiss(src_op.as<x86::Xmm>(), limit);
		    pgm.cc.jp(direct_convert);
		    pgm.cc.jb(direct_convert);
		    x86::Xmm adj = pgm.cc.newXmmSs("cast_r2u32_adj");
		    pgm.cc.movss(adj, src_op.as<x86::Xmm>());
		    pgm.cc.subss(adj, limit);
		    pgm.cc.cvttss2si(out.r32(), adj);
		}
		else
		{
		    x86::Mem limit = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, 2147483648.0);
		    limit.setSize(sizeof(double));
		    pgm.cc.ucomisd(src_op.as<x86::Xmm>(), limit);
		    pgm.cc.jp(direct_convert);
		    pgm.cc.jb(direct_convert);
		    x86::Xmm adj = pgm.cc.newXmmSd("cast_r2u32_adj");
		    pgm.cc.movsd(adj, src_op.as<x86::Xmm>());
		    pgm.cc.subsd(adj, limit);
		    pgm.cc.cvttsd2si(out.r32(), adj);
		}
		pgm.cc.add(out.r32(), imm(0x80000000u));
		pgm.cc.jmp(convert_done);
		pgm.cc.bind(direct_convert);
		if ( src_type->size == sizeof(float) )
		    pgm.cc.cvttss2si(out.r32(), src_op.as<x86::Xmm>());
		else
		    pgm.cc.cvttsd2si(out.r32(), src_op.as<x86::Xmm>());
		pgm.cc.bind(convert_done);
	    }
	    else
	    {
	    bool clamp_positive_i32 = !cast_type->is_unsigned() && cast_type->size == 4;
	    Label clamp_done = pgm.cc.newLabel();
	    Label do_convert = pgm.cc.newLabel();
	    if ( clamp_positive_i32 )
	    {
		if ( src_type->size == sizeof(float) )
		{
		    x86::Mem limit = pgm.cc.newFloatConst(ConstPoolScope::kLocal, 2147483648.0f);
		    limit.setSize(sizeof(float));
		    pgm.cc.ucomiss(src_op.as<x86::Xmm>(), limit);
		}
		else
		{
		    x86::Mem limit = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, 2147483648.0);
		    limit.setSize(sizeof(double));
		    pgm.cc.ucomisd(src_op.as<x86::Xmm>(), limit);
		}
		pgm.cc.jp(do_convert);
		pgm.cc.jb(do_convert);
		pgm.cc.mov(out.r32(), imm(INT32_MAX));
		pgm.cc.jmp(clamp_done);
		pgm.cc.bind(do_convert);
	    }
	    if ( src_type->size == sizeof(float) )
		pgm.cc.cvttss2si(out, src_op.as<x86::Xmm>());
	    else
		pgm.cc.cvttsd2si(out, src_op.as<x86::Xmm>());
	    if ( clamp_positive_i32 )
		pgm.cc.bind(clamp_done);
	    }
	}
	else if ( src_op.isMem() )
	{
	    if ( unsigned_u32 )
	    {
		x86::Xmm src_tmp = newScalarXmm(pgm, src_type, "cast_r2u32_mem");
		if ( src_type->size == sizeof(float) )
		    pgm.cc.movss(src_tmp, src_op.as<x86::Mem>());
		else
		    pgm.cc.movsd(src_tmp, src_op.as<x86::Mem>());
		Label direct_convert = pgm.cc.newLabel();
		Label convert_done = pgm.cc.newLabel();
		if ( src_type->size == sizeof(float) )
		{
		    x86::Mem limit = pgm.cc.newFloatConst(ConstPoolScope::kLocal, 2147483648.0f);
		    limit.setSize(sizeof(float));
		    pgm.cc.ucomiss(src_tmp, limit);
		    pgm.cc.jp(direct_convert);
		    pgm.cc.jb(direct_convert);
		    x86::Xmm adj = pgm.cc.newXmmSs("cast_r2u32_adj");
		    pgm.cc.movss(adj, src_tmp);
		    pgm.cc.subss(adj, limit);
		    pgm.cc.cvttss2si(out.r32(), adj);
		}
		else
		{
		    x86::Mem limit = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, 2147483648.0);
		    limit.setSize(sizeof(double));
		    pgm.cc.ucomisd(src_tmp, limit);
		    pgm.cc.jp(direct_convert);
		    pgm.cc.jb(direct_convert);
		    x86::Xmm adj = pgm.cc.newXmmSd("cast_r2u32_adj");
		    pgm.cc.movsd(adj, src_tmp);
		    pgm.cc.subsd(adj, limit);
		    pgm.cc.cvttsd2si(out.r32(), adj);
		}
		pgm.cc.add(out.r32(), imm(0x80000000u));
		pgm.cc.jmp(convert_done);
		pgm.cc.bind(direct_convert);
		if ( src_type->size == sizeof(float) )
		    pgm.cc.cvttss2si(out.r32(), src_tmp);
		else
		    pgm.cc.cvttsd2si(out.r32(), src_tmp);
		pgm.cc.bind(convert_done);
	    }
	    else
	    {
	    bool clamp_positive_i32 = !cast_type->is_unsigned() && cast_type->size == 4;
	    Label clamp_done = pgm.cc.newLabel();
	    Label do_convert = pgm.cc.newLabel();
	    if ( clamp_positive_i32 )
	    {
		x86::Xmm src_tmp = newScalarXmm(pgm, src_type, "cast_r2i_cmp");
		if ( src_type->size == sizeof(float) )
		    pgm.cc.movss(src_tmp, src_op.as<x86::Mem>());
		else
		    pgm.cc.movsd(src_tmp, src_op.as<x86::Mem>());
		if ( src_type->size == sizeof(float) )
		{
		    x86::Mem limit = pgm.cc.newFloatConst(ConstPoolScope::kLocal, 2147483648.0f);
		    limit.setSize(sizeof(float));
		    pgm.cc.ucomiss(src_tmp, limit);
		}
		else
		{
		    x86::Mem limit = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, 2147483648.0);
		    limit.setSize(sizeof(double));
		    pgm.cc.ucomisd(src_tmp, limit);
		}
		pgm.cc.jp(do_convert);
		pgm.cc.jb(do_convert);
		pgm.cc.mov(out.r32(), imm(INT32_MAX));
		pgm.cc.jmp(clamp_done);
		pgm.cc.bind(do_convert);
	    }
	    if ( src_type->size == sizeof(float) )
		pgm.cc.cvttss2si(out, src_op.as<x86::Mem>());
	    else
		pgm.cc.cvttsd2si(out, src_op.as<x86::Mem>());
	    if ( clamp_positive_i32 )
		pgm.cc.bind(clamp_done);
	    }
	}
	else
	{
	    // Non-real operand (int masquerading as real) — just mov.
	    pgm.cc.mov(out, src_op.isReg() ? src_op.as<x86::Gp>()
					    : src_op.as<x86::Gp>());
	}
	if ( unsigned_u32 )
	{
	    regdp.second = cast_type;
	    if ( regdp.first
	      && !(regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec)) )
	    {
		Operand out32 = out.r32();
		pgm.safemov(*regdp.first, out32, cast_type, cast_type);
		return *regdp.first;
	    }
	    _operand = out.r32();
	    regdp.first = &_operand;
	    return _operand;
	}
	// Narrow-integer canonicalization (e.g. int32 → movsxd).
	IRBuilder ir(pgm.cc);
	IRValue result = ir.coerce(IRValue::reg(out, &ddINT64), cast_type);
	result = ir.load(result);
	regdp.second = cast_type;
	// Write to caller dest if compatible. Skip Xmm destinations:
	// safemov(Xmm, Gp) does cvtsi2ss, but the caller (e.g.
	// compile_token_normalized) will do its own int→float coerce
	// and would double-convert if we wrote here.
	if ( regdp.first
	  && !(regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec)) )
	{
	    pgm.safemov(*regdp.first, result.op, cast_type, cast_type);
	    return *regdp.first;
	}
	_operand = result.op;
	regdp.first = &_operand;
	return _operand;
    }

    // Integer → real cast: compile inner as int, convert to Xmm.
    // Do NOT force the inner expression to src_type — that would
    // override an unsigned operand's type with signed ddINT32 and
    // make `>>` use SAR instead of SHR.  Let the expression infer
    // its own type; we pick up the actual type afterward.
    if ( dst_is_real && src_type && src_type->is_integer() )
    {
	regdefp_t inner_rdp = {NULL, NULL, NULL};
	Operand &src_op = expr->compile(pgm, inner_rdp);
	DataDef *actual_src = inner_rdp.second ? inner_rdp.second : src_type;
	if ( cast_type->is_simd() && actual_src && actual_src->is_simd() )
	{
	    x86::Xmm out = cast_type->newreg(pgm.cc, "cast_simd_i2r").as<x86::Xmm>();
	    if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.safemov(out, src_op.as<x86::Xmm>(), cast_type, actual_src);
	    else if ( src_op.isMem() )
	    {
		x86::Mem mem = src_op.as<x86::Mem>();
		load_mem_to_xmm(pgm, out, mem, actual_src);
	    }
	    else if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.movq(out, src_op.as<x86::Gp>());
	    else
		throw "TokenCast::compile() SIMD int->real expects Xmm/Mem/Gp source";
	    regdp.second = cast_type;
	    if ( regdp.first )
	    {
		pgm.safemov(*regdp.first, *(Operand *)&out, cast_type, cast_type);
		return *regdp.first;
	    }
	    _operand = out;
	    regdp.first = &_operand;
	    return _operand;
	}
	x86::Xmm out = newScalarXmm(pgm, cast_type, "cast_i2r");
	x86::Gp gp;
	if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	    gp = src_op.as<x86::Gp>();
	else if ( src_op.isMem() )
	{
	    gp = pgm.cc.newGpq("cast_i2r_tmp");
	    pgm.cc.mov(gp, src_op.as<x86::Mem>());
	}
	else
	    gp = src_op.as<x86::Gp>(); // fallback

	// Unsigned 64-bit needs special handling: cvtsi2sd treats the
	// value as signed. For uint64 with high bit set, use the GCC
	// pattern: test sign, if negative shift right by 1, OR in the
	// low bit, convert, then double the result.
	bool src_unsigned = actual_src && actual_src->is_unsigned();
	if ( actual_src && !src_unsigned && actual_src->size == 4 )
	{
	    x86::Gp signed_gp = pgm.cc.newGpq("cast_i32_signext");
	    pgm.cc.movsxd(signed_gp, gp.r32());
	    gp = signed_gp;
	}
	if ( src_unsigned && actual_src->size == 8 )
	{
	    Label lbl_positive = pgm.cc.newLabel();
	    Label lbl_done = pgm.cc.newLabel();
	    pgm.cc.test(gp.r64(), gp.r64());
	    pgm.cc.jns(lbl_positive);
	    // High bit set: (double)(val >> 1 | (val & 1)) * 2.0
	    x86::Gp tmp = pgm.cc.newGpq("u64_halved");
	    pgm.cc.mov(tmp, gp.r64());
	    x86::Gp low_bit = pgm.cc.newGpq("u64_low");
	    pgm.cc.mov(low_bit, gp.r64());
	    pgm.cc.and_(low_bit, imm(1));
	    pgm.cc.shr(tmp, imm(1));
	    pgm.cc.or_(tmp, low_bit);
	    if ( cast_type->size == sizeof(float) )
	    {
		pgm.cc.cvtsi2ss(out, tmp.r64());
		pgm.cc.addss(out, out);
	    }
	    else
	    {
		pgm.cc.cvtsi2sd(out, tmp.r64());
		pgm.cc.addsd(out, out);
	    }
	    pgm.cc.jmp(lbl_done);
	    // Positive: simple signed conversion
	    pgm.cc.bind(lbl_positive);
	    if ( cast_type->size == sizeof(float) )
		pgm.cc.cvtsi2ss(out, gp.r64());
	    else
		pgm.cc.cvtsi2sd(out, gp.r64());
	    pgm.cc.bind(lbl_done);
	}
	else if ( cast_type->size == sizeof(float) )
	    pgm.cc.cvtsi2ss(out, gp.r64());
	else
	    pgm.cc.cvtsi2sd(out, gp.r64());
	regdp.second = cast_type;
	if ( regdp.first )
	{
	    pgm.safemov(*regdp.first, *(Operand *)&out, cast_type, cast_type);
	    return *regdp.first;
	}
	_operand = out;
	regdp.first = &_operand;
	return _operand;
    }

    // Integer narrowing / truncation cast: compile at the source width,
    // then truncate via IR coerce.  Handles explicit narrowing (int64 →
    // int32) and same-size sign changes (signed → unsigned).
    //
    // When src_type is known and wider than cast_type, or signedness
    // differs at the same sub-8 width, use the narrowing path.
    // When src_type is unknown or equals cast_type AND the inner expr
    // is an operator (which always reports ddINT, now 4 bytes, regardless
    // of actual operand widths), compile with NULL type so the inner
    // expression computes at its natural width, then truncate.
    {
	bool known_narrow = src_type && src_type->is_integer()
	    && cast_type && cast_type->is_integer()
	    && (cast_type->size < src_type->size
	     || (cast_type->size < 8 && cast_type->size == src_type->size
		 && cast_type->is_unsigned() != src_type->is_unsigned()));
	// Operator expressions always report ddINT as their datadef, but
	// may compute wider values (e.g. LL_literal >> n).  Detect this
	// and force the natural-width compile path.
	bool operator_may_be_wider = !known_narrow
	    && cast_type && cast_type->is_integer() && cast_type->size < 8
	    && src_type == &ddINT
	    && dynamic_cast<TokenOperator *>(expr) != nullptr;

	if ( known_narrow || operator_may_be_wider )
	{
	    regdefp_t inner_rdp = {NULL, NULL, NULL};
	    Operand &src_op = expr->compile(pgm, inner_rdp);
	    DataDef *actual_src = inner_rdp.second ? inner_rdp.second
				: (src_type && src_type->is_integer() ? src_type : &ddINT64);
	    if ( actual_src->size > cast_type->size
	      || (actual_src->size == cast_type->size && actual_src->size < 8
		  && actual_src->is_unsigned() != cast_type->is_unsigned()) )
	    {
		IRBuilder ir(pgm.cc);
		IRValue out = ir.coerce(ir_from_operand(src_op, actual_src), cast_type);
		out = ir.load(out);
		regdp.second = cast_type;
		if ( regdp.first )
		{
		    pgm.safemov(*regdp.first, out.op, cast_type, cast_type);
		    return *regdp.first;
		}
		_operand = out.op;
		regdp.first = &_operand;
		return _operand;
	    }
	    // No truncation needed — just relabel. If the caller provided
	    // a destination, store the value there.
	    regdp.second = cast_type;
	    if ( regdp.first )
	    {
		pgm.safemov(*regdp.first, src_op, cast_type, actual_src);
		return *regdp.first;
	    }
	    regdp.first = &src_op;
	    return src_op;
	}
    }

    // Compile the inner expression into a fresh regdp so nested casts
    // (e.g. (long long)(int)a) don't leak the caller's destination and
    // skip the narrowing step.
    {
	Operand *caller_dest = regdp.first;
	DataDef *inner_hint = cast_type;
	// Integer casts apply after the source expression's own arithmetic.
	// Do not force the inner expression to the destination width here,
	// or `(long)(u32_a + u32_b)` stops wrapping in 32 bits before widen.
	if ( cast_type && cast_type->is_integer() && !cast_type->is_simd()
	  && src_type && src_type->is_integer() && !src_type->is_simd() )
	    inner_hint = NULL;
	regdefp_t inner_rdp = {NULL, inner_hint, NULL};
	Operand &result = expr->compile(pgm, inner_rdp);
	DataDef *actual_inner = inner_rdp.second ? inner_rdp.second : cast_type;
	regdp.second = cast_type;

	// Widen if the inner produced a narrower register (e.g. Gpd from
	// an inner (int) cast needs sign-extension to Gpq for (long long)).
	if ( cast_type->is_integer() && actual_inner->is_integer()
	  && actual_inner->size < cast_type->size
	  && result.isReg() && result.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    IRBuilder ir(pgm.cc);
	    IRValue widened = ir.coerce(IRValue::reg(result, actual_inner), cast_type);
	    widened = ir.load(widened);
	    if ( caller_dest )
	    {
		pgm.safemov(*caller_dest, widened.op, cast_type, cast_type);
		regdp.first = caller_dest;
		return *caller_dest;
	    }
	    _operand = widened.op;
	    regdp.first = &_operand;
	    return _operand;
	}

	if ( caller_dest )
	{
	    pgm.safemov(*caller_dest, result, cast_type, cast_type);
	    regdp.first = caller_dest;
	    return *caller_dest;
	}
	regdp.first = &result;
	return result;
    }
}

// compile ternary operator: condition ? true_expr : false_expr
// Uses a stack slot as the merge point to avoid asmjit register allocator
// issues with the same virtual register written on two divergent paths.
Operand &TokenTerQ::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenTerQ::compile() TOP" << std::endl);
    DBG(pgm.cc.comment("ternary ? : start"));
    Label L_false = pgm.cc.newLabel();
    Label L_end   = pgm.cc.newLabel();

    // Propagate the branch type to the caller. Parser already stored it on
    // _datatype (true branch, fallback to false branch). Without this,
    // variadic-arg coercion paths (e.g. printf's dtSTRING → char* via
    // string_cstr) can't see the ternary as a string-typed expression and
    // pass a raw std::string* to %s, producing garbage.
    if ( !regdp.second )
	regdp.second = _datatype ? _datatype : &ddINT64;

    Operand *caller_dest = regdp.first;

    // Compile the condition to a testable operand. Keep reals in Xmm so
    // `testzero()` can use `ucomisd`, but materialize immediates.
    regdefp_t condrdp = {NULL, NULL, NULL};
    Operand cond_storage;
    Operand &raw_cond = condition->compile(pgm, condrdp);
    DataDef *cond_type = condrdp.second ? condrdp.second
	: (condition && condition->datadef() ? condition->datadef() : &ddINT64);
    Operand *cond_op = &raw_cond;
    if ( cond_type && (cond_type->is_numeric() || cond_type->is_pointer())
      && (raw_cond.isReg() || raw_cond.isMem() || raw_cond.isImm()) )
    {
	IRBuilder ir(pgm.cc);
	IRValue cond_value = ir.load(ir.coerce(ir_from_operand(raw_cond, cond_type), cond_type));
	cond_storage = cond_value.op;
	cond_op = &cond_storage;
    }
    else if ( raw_cond.isImm() )
    {
	x86::Gp cond_gp = pgm.cc.newGpq("__tern_cond");
	pgm.cc.mov(cond_gp, raw_cond.as<Imm>());
	cond_storage = cond_gp;
	cond_op = &cond_storage;
    }
    pgm.testzero(*cond_op);
    pgm.cc.je(L_false);

    // Pointer-sized merge surface: numeric, true pointer, OR string
    // (std::string is held as an 8-byte pointer-to-object, so it fits
    // the same merge slot). Routing dtSTRING here lets a mixed
    // string-literal/char* ternary unify cleanly via compile_token_normalized
    // — each branch is coerced to the merge type through IRBuilder so the
    // false branch's raw char* and the true branch's std::string both
    // arrive in the slot as the same shape.
    if ( regdp.second
      && (regdp.second->is_numeric() || regdp.second->is_pointer() || regdp.second->is_string()) )
    {
	uint32_t merge_size = (uint32_t)(regdp.second->size ? regdp.second->size : 8);
	if ( regdp.second->is_string() )
	    merge_size = 8;
	uint32_t merge_align = merge_size < 8 ? merge_size : 8;
	x86::Mem merge_slot = pgm.cc.newStack(merge_size, merge_align);
	merge_slot.setSize(merge_size);

	auto emit_branch = [&](TokenBase *expr) {
	    if ( type_is_cstr_pointer(regdp.second) )
	    {
		Operand branch_storage;
		Operand &norm = compile_token_normalized(pgm, expr, regdp.second, nullptr, branch_storage);
		IRBuilder ir(pgm.cc);
		ir.store(IRValue::mem(merge_slot, regdp.second), ir_from_operand(norm, regdp.second));
		return;
	    }

	    regdefp_t brdp = {NULL, NULL, NULL};
	    Operand &raw = expr->compile(pgm, brdp);
	    DataDef *raw_type = brdp.second
		? brdp.second
		: (expr && expr->datadef() ? expr->datadef() : regdp.second);
	    // Fixed-array branches decay to pointer-to-element here. A
	    // TokenVar of `char buf[N]` reports `char` as its element
	    // type but voperand returns a Gp holding the array's
	    // address; treat its raw type as the corresponding
	    // pointer-to-element so the IR coerce path sees an 8-byte
	    // pointer instead of a 1-byte scalar. Only relabel when
	    // the merge surface itself is pointer-like — int merges of
	    // genuine scalar fixed-array elements still want the scalar.
	    if ( regdp.second && (regdp.second->is_pointer() || regdp.second->is_string()) )
	    {
		bool branch_decays = false;
		if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
		    if ( tv->var.is_fixed_array() )
			branch_decays = true;
		if ( TokenMember *tm = dynamic_cast<TokenMember *>(expr) )
		    if ( tm->is_fixed_array_member() )
			branch_decays = true;
		if ( expr && dynamic_cast<DataDefCArray *>(expr->datadef()) )
		    branch_decays = true;
		if ( branch_decays && raw_type && !raw_type->is_pointer() )
		{
		    DataDef *decay_ptr = effective_pointer_type_for_arith(pgm, expr);
		    raw_type = decay_ptr ? decay_ptr : &ddLPSTR;
		}
	    }
	    // If the branch returns void (e.g. abort()), skip the
	    // merge-slot store — the call either never returns or
	    // produces no meaningful value.
	    if ( raw_type && raw_type->type() == DataType::dtVOID )
		return;
	    IRBuilder ir(pgm.cc);
	    IRValue coerced = ir.coerce(ir_from_operand(raw, raw_type), regdp.second);
	    ir.store(IRValue::mem(merge_slot, regdp.second), coerced);
	};

	emit_branch(true_expr);
	pgm.cc.jmp(L_end);

	pgm.cc.bind(L_false);
	emit_branch(false_expr);
	pgm.cc.bind(L_end);

	// emit_branch coerced each branch into the merge slot at
	// regdp.second's type — that's also what we're returning.  Update
	// the ternary's reported parse-time datadef so callers (notably
	// compile_call_arg_normalized's want_cstr check) see the coerced
	// type instead of the original branch type.  Without this a
	// `cond ? "a" : "b"` passed to a `const char *` parameter got
	// double-coerced: TokenTerQ produced a c_str pointer, then the
	// call-site re-ran string_cstr() on it (treating the char* as a
	// std::string*), and the resulting "pointer" was actually the
	// first 8 bytes of the string data — a guaranteed SIGSEGV on
	// deref.
	if ( regdp.second )
	    _datatype = regdp.second;

	regdp.first = caller_dest;
	return emit_ir_value(pgm, IRValue::mem(merge_slot, regdp.second), regdp, _operand, regdp.second);
    }

    // Non-numeric fallback: keep the legacy register-merge path for now.
    x86::Gp result = pgm.cc.newGpq("__tern_result");
    {
	regdefp_t trdp = {&_operand, regdp.second, NULL};
	_operand = result;
	true_expr->compile(pgm, trdp);
	if ( !regdp.second && trdp.second )
	    regdp.second = trdp.second;
    }
    pgm.cc.jmp(L_end);

    pgm.cc.bind(L_false);
    {
	regdefp_t frdp = {&_operand, regdp.second, NULL};
	_operand = result;
	false_expr->compile(pgm, frdp);
    }
    pgm.cc.bind(L_end);

    _operand = result;
    if ( caller_dest )
    {
	if ( caller_dest->isReg() && caller_dest->as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    if ( caller_dest->id() != _operand.id() )
		pgm.cc.mov(caller_dest->as<x86::Gp>(), result);
	    return *caller_dest;
	}
	if ( caller_dest->isMem() )
	{
	    IRBuilder ir(pgm.cc);
	    ir.store(IRValue::mem(caller_dest->as<x86::Mem>(), regdp.second),
		     ir_from_operand(result, regdp.second));
	    return *caller_dest;
	}
    }
    regdp.first = &_operand;
    return _operand;
}

Operand &TokenTypeQuery::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !regdp.second )
	regdp.second = &ddUINT64;

    if ( !query_type || want_alignof )
	return emit_ir_value(pgm,
	    IRValue::imm(Imm((int64_t)(query_type ? query_type->alignment() : 0)), &ddUINT64),
	    regdp, _operand, &ddUINT64);

    if ( DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(query_type) )
    {
	if ( use_cached_runtime_size && sdd->runtime_size_expr )
	{
	    regdefp_t size_rdp = {NULL, &ddUINT64, NULL};
	    Operand &size_op = sdd->runtime_size_expr->compile(pgm, size_rdp);
	    return emit_ir_value(pgm, ir_from_operand(size_op, &ddUINT64), regdp, _operand, &ddUINT64);
	}
	if ( sdd->has_runtime_size() )
	{
	    x86::Gp total = emit_runtime_struct_size(pgm, sdd, "sizeof_dyn_struct");
	    return emit_ir_value(pgm, IRValue::reg(total, &ddUINT64), regdp, _operand, &ddUINT64);
	}
    }
    else if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(query_type) )
    {
	if ( add->count_expr )
	{
	    regdefp_t count_rdp = {NULL, NULL, NULL};
	    Operand &count_op = add->count_expr->compile(pgm, count_rdp);
	    x86::Gp total = pgm.cc.newGpq("sizeof_dyn_array");
	    load_idx_to_gpq(pgm, total, count_op);
	    size_t elem_size = add->element_type ? add->element_type->size : 0;
	    if ( elem_size > 1 )
		pgm.cc.imul(total, total, imm((int64_t)elem_size));
	    return emit_ir_value(pgm, IRValue::reg(total, &ddUINT64), regdp, _operand, &ddUINT64);
	}
    }

    return emit_ir_value(pgm,
	IRValue::imm(Imm((int64_t)query_type->size), &ddUINT64),
	regdp, _operand, &ddUINT64);
}

