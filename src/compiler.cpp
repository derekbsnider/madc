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
#include <dlfcn.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <functional>
#include <complex>
#include <map>
#include <set>
#include <list>
#include <vector>
#include <queue>
#include <stack>
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

const char *string_cstr(void *ptr);

extern "C" void *madc_runtime_memcpy(void *dst, const void *src, size_t n)
{
    return ::memcpy(dst, src, n);
}

extern "C" void madc_runtime_complex_div_float(void *out,
					       float ar, float ai,
					       float br, float bi)
{
    std::complex<float> result = std::complex<float>(ar, ai)
			       / std::complex<float>(br, bi);
    float *parts = static_cast<float *>(out);
    parts[0] = result.real();
    parts[1] = result.imag();
}

extern "C" void madc_runtime_complex_div_double(void *out,
					        double ar, double ai,
					        double br, double bi)
{
    std::complex<double> result = std::complex<double>(ar, ai)
				/ std::complex<double>(br, bi);
    double *parts = static_cast<double *>(out);
    parts[0] = result.real();
    parts[1] = result.imag();
}

// Materialize an operand into a Gp register. If already Gp, return it.
// If Mem (global struct/class), LEA the address into a fresh Gp.
x86::Gp as_gp_ptr(Program &pgm, Operand &op, const char *name)
{
    if ( op.isReg() && op.as<BaseReg>().isGroup(RegGroup::kGp) )
	return op.as<x86::Gp>();
    if ( op.isMem() )
    {
	x86::Gp tmp = pgm.cc.newIntPtr("%s", name);
	pgm.cc.lea(tmp, op.as<x86::Mem>());
	return tmp;
    }
    throw "as_gp_ptr: operand is neither Gp nor Mem";
}


x86::Gp materialize_gp_ptr_arg(Program &pgm, Operand &op, const char *name)
{
    x86::Gp src = as_gp_ptr(pgm, op, name);
    x86::Gp arg = pgm.cc.newIntPtr("%s_arg", name);
    pgm.cc.mov(arg, src);
    return arg;
}

static void emit_struct_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
    DataDefSTRUCT *dds, const std::vector<TokenBase *> &inits, TokenBase *err_loc);
static x86::Gp emit_runtime_aggregate_size(Program &pgm, DataDef *copy_type, const char *name);
static void emit_runtime_aggregate_copy(Program &pgm, Operand &dst, Operand &src,
					x86::Gp &size_gp, const char *name);
static size_t internal_vararg_static_slot_size(DataDef *type);
static const char *token_constant_cstring(TokenBase *token);

static int64_t token_pointer_element_size(TokenBase *tb)
{
    if ( !tb )
	return 1;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) )
    {
	if ( tv->var.is_fixed_array() && tv->var.type )
	    return tv->var.type->size ? (int64_t)tv->var.type->size : 1;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(tb) )
    {
	if ( tm->is_fixed_array_member() && tm->var.type )
	    return tm->var.type->size ? (int64_t)tm->var.type->size : 1;
    }
    DataDef *dd = tb->datadef();
    if ( DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(dd) )
	return (pdd->base_type && pdd->base_type->size) ? (int64_t)pdd->base_type->size : 1;
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(dd) )
	return (add->element_type && add->element_type->size) ? (int64_t)add->element_type->size : 1;
    return (dd && dd->size) ? (int64_t)dd->size : 1;
}

static int64_t fixed_array_object_size(const Variable &var)
{
    if ( !var.is_fixed_array() || !var.type || !var.type->size )
	return -1;
    return (int64_t)var.type->size * (int64_t)var.total_elements();
}

static int64_t remaining_object_size(int64_t base_size, int64_t index, int64_t elem_size)
{
    if ( base_size < 0 || index < 0 || elem_size <= 0 )
	return -1;
    int64_t used = index * elem_size;
    if ( used >= base_size )
	return 0;
    return base_size - used;
}

bool try_eval_const_i64(TokenBase *tb, int64_t &out)
{
    if ( !tb )
	return false;
    if ( TokenInt *ti = dynamic_cast<TokenInt *>(tb) )
    {
	out = ti->ival();
	return true;
    }
    if ( TokenChar *tc = dynamic_cast<TokenChar *>(tb) )
    {
	out = tc->ival();
	return true;
    }
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) )
    {
	if ( tv->var.is_constant() )
	{
	    out = tv->var.get<int64_t>();
	    return true;
	}
	return false;
    }
    if ( TokenNeg *tn = dynamic_cast<TokenNeg *>(tb) )
    {
	int64_t rhs = 0;
	if ( try_eval_const_i64(tn->right, rhs) )
	{
	    out = -rhs;
	    return true;
	}
	return false;
    }
    if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(tb) )
    {
	int64_t lhs = 0, rhs = 0;
	if ( try_eval_const_i64(ta->left, lhs) && try_eval_const_i64(ta->right, rhs) )
	{
	    out = lhs + rhs;
	    return true;
	}
	return false;
    }
    if ( TokenSub *ts = dynamic_cast<TokenSub *>(tb) )
    {
	int64_t lhs = 0, rhs = 0;
	if ( try_eval_const_i64(ts->left, lhs) && try_eval_const_i64(ts->right, rhs) )
	{
	    out = lhs - rhs;
	    return true;
	}
	return false;
    }
    if ( TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(tb) )
    {
	if ( tcf->var.name == "__builtin_object_size" && tcf->argc() == 2 )
	{
	    int64_t mode = 0;
	    if ( !try_eval_const_i64(tcf->parameters[1], mode) )
		mode = 0;
	    int64_t size = estimate_object_size(tcf->parameters[0], (int)mode);
	    out = size >= 0 ? size : -1;
	    return true;
	}
    }
    return false;
}

static int64_t estimate_lvalue_object_size(TokenBase *expr, int mode)
{
    if ( !expr )
	return -1;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
    {
	int64_t array_size = fixed_array_object_size(tv->var);
	if ( array_size >= 0 )
	    return array_size;
	return tv->var.type ? (int64_t)tv->var.type->size : -1;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(expr) )
    {
	if ( tm->is_fixed_array_member() )
	{
	    DataDefSTRUCT *sdd = tm->owner_struct_type();
	    std::string mname = tm->var.name;
	    if ( sdd && tm->var.type )
		return (int64_t)sdd->m_count(mname) * (int64_t)tm->var.type->size;
	}
	return tm->var.type ? (int64_t)tm->var.type->size : -1;
    }
    if ( TokenSubscript *ts = dynamic_cast<TokenSubscript *>(expr) )
    {
	int64_t index = 0;
	if ( !try_eval_const_i64(ts->index, index) )
	    return -1;
	int64_t base_size = ts->object.is_fixed_array()
	    ? fixed_array_object_size(ts->object)
	    : ts->object.object_size_hint;
	return remaining_object_size(base_size, index, ts->datadef() ? (int64_t)ts->datadef()->size : 1);
    }
    if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(expr) )
    {
	int64_t index = 0;
	if ( !try_eval_const_i64(tse->index, index) )
	    return -1;
	int64_t base_size = estimate_object_size(tse->base_expr, mode);
	return remaining_object_size(base_size, index, tse->datadef() ? (int64_t)tse->datadef()->size : 1);
    }
    return expr->datadef() ? (int64_t)expr->datadef()->size : -1;
}

int64_t estimate_object_size(TokenBase *expr, int mode)
{
    if ( !expr )
	return -1;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
    {
	int64_t array_size = fixed_array_object_size(tv->var);
	if ( array_size >= 0 )
	    return array_size;
	return tv->var.object_size_hint;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(expr) )
    {
	if ( tm->is_fixed_array_member() )
	{
	    DataDefSTRUCT *sdd = tm->owner_struct_type();
	    std::string mname = tm->var.name;
	    if ( sdd && tm->var.type )
		return (int64_t)sdd->m_count(mname) * (int64_t)tm->var.type->size;
	}
	return tm->var.object_size_hint;
    }
    if ( TokenAddrOf *tao = dynamic_cast<TokenAddrOf *>(expr) )
    {
	int64_t array_size = fixed_array_object_size(tao->var);
	if ( array_size >= 0 )
	    return array_size;
	return tao->var.type ? (int64_t)tao->var.type->size : -1;
    }
    if ( TokenAddrExpr *tae = dynamic_cast<TokenAddrExpr *>(expr) )
	return estimate_lvalue_object_size(tae->expr, mode);
    if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(expr) )
    {
	int64_t offset = 0;
	if ( try_eval_const_i64(ta->right, offset) )
	{
	    int64_t base_size = estimate_object_size(ta->left, mode);
	    return remaining_object_size(base_size, offset, token_pointer_element_size(ta->left));
	}
	if ( try_eval_const_i64(ta->left, offset) )
	{
	    int64_t base_size = estimate_object_size(ta->right, mode);
	    return remaining_object_size(base_size, offset, token_pointer_element_size(ta->right));
	}
	return -1;
    }
    if ( TokenSub *ts = dynamic_cast<TokenSub *>(expr) )
    {
	int64_t offset = 0;
	if ( try_eval_const_i64(ts->right, offset) )
	{
	    int64_t base_size = estimate_object_size(ts->left, mode);
	    return remaining_object_size(base_size, offset, token_pointer_element_size(ts->left));
	}
	return -1;
    }
    if ( TokenTerQ *tt = dynamic_cast<TokenTerQ *>(expr) )
    {
	int64_t true_size = estimate_object_size(tt->true_expr, mode);
	int64_t false_size = estimate_object_size(tt->false_expr, mode);
	if ( true_size < 0 || false_size < 0 )
	    return -1;
	return mode == 1 ? std::min(true_size, false_size)
			 : std::max(true_size, false_size);
    }
    if ( TokenCast *tc = dynamic_cast<TokenCast *>(expr) )
	return estimate_object_size(tc->expr, mode);
    if ( TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(expr) )
    {
	if ( (tcf->var.name == "__builtin_alloca" || tcf->var.name == "alloca" || tcf->var.name == "malloc")
	  && tcf->argc() == 1 )
	{
	    int64_t size = 0;
	    if ( try_eval_const_i64(tcf->parameters[0], size) && size >= 0 )
		return size;
	}
    }
    return -1;
}

Operand &redirect_builtin_call(Program &pgm, const std::string &target_name,
				      const std::vector<TokenBase *> &args,
				      regdefp_t &regdp, TokenCallFunc *site,
				      Operand &site_operand)
{
    std::string lookup = target_name;
    Variable *target = pgm.findVariable(lookup);
    if ( (!target || !target->type || !target->type->is_function())
      && pgm.is_dynamic_symbol_fallback_enabled()
      && pgm.is_dynamic_symbol_allowed(target_name) )
    {
	void *sym = dlsym(RTLD_DEFAULT, target_name.c_str());
	if ( sym )
	{
	    target = pgm.addFunction(target_name,
		datatype_vec_t{DataType::dtINT64},
		(fVOIDFUNC)sym);
	    if ( !target )
		target = pgm.findVariable(lookup);
	}
    }
    if ( !target || !target->type || !target->type->is_function() )
	pgm.Throw(site) << "missing target for " << site->var.name << flush;
    TokenCallFunc redirected(*target);
    redirected.file = site->file;
    redirected.line = site->line;
    redirected.column = site->column;
    redirected.parameters = args;
    Operand &redirected_result = redirected.compile(pgm, regdp);
    if ( regdp.first == &redirected_result || !regdp.first )
    {
	site_operand = redirected_result;
	regdp.first = &site_operand;
	return site_operand;
    }
    return *regdp.first;
}

// Overflow and builtin dispatch moved to compiler_builtins.cpp.

static TypeId simd_type_id(DataDefSIMD *vdd)
{
    if ( !vdd || !vdd->element_type )
	return TypeId::kVoid;
    bool is_real = vdd->element_type->is_real();
    bool is_unsigned = vdd->element_type->is_unsigned();
    size_t elem = vdd->element_type->size;
    size_t bytes = vdd->size;

    if ( bytes == 4 )
    {
	if ( elem == 1 ) return is_unsigned ? TypeId::kUInt8x4  : TypeId::kInt8x4;
	if ( elem == 2 ) return is_unsigned ? TypeId::kUInt16x2 : TypeId::kInt16x2;
	if ( elem == 4 ) return is_unsigned ? TypeId::kUInt32x1 : TypeId::kInt32x1;
    }
    if ( bytes == 8 )
    {
	if ( is_real )
	{
	    if ( elem == 4 ) return TypeId::kFloat32x2;
	    if ( elem == 8 ) return TypeId::kFloat64x1;
	}
	else
	{
	    if ( elem == 1 ) return is_unsigned ? TypeId::kUInt8x8  : TypeId::kInt8x8;
	    if ( elem == 2 ) return is_unsigned ? TypeId::kUInt16x4 : TypeId::kInt16x4;
	    if ( elem == 4 ) return is_unsigned ? TypeId::kUInt32x2 : TypeId::kInt32x2;
	    if ( elem == 8 ) return is_unsigned ? TypeId::kUInt64x1 : TypeId::kInt64x1;
	}
    }
    if ( bytes == 16 )
    {
	if ( is_real )
	{
	    if ( elem == 4 ) return TypeId::kFloat32x4;
	    if ( elem == 8 ) return TypeId::kFloat64x2;
	}
	else
	{
	    if ( elem == 1 ) return is_unsigned ? TypeId::kUInt8x16  : TypeId::kInt8x16;
	    if ( elem == 2 ) return is_unsigned ? TypeId::kUInt16x8 : TypeId::kInt16x8;
	    if ( elem == 4 ) return is_unsigned ? TypeId::kUInt32x4 : TypeId::kInt32x4;
	    if ( elem == 8 ) return is_unsigned ? TypeId::kUInt64x2 : TypeId::kInt64x2;
	}
    }
    return TypeId::kVoid;
}

static size_t count_struct_init_slots(DataDefSTRUCT *dds)
{
    if ( !dds )
	return 0;
    size_t total = 0;
    for ( size_t mi = 0; mi < dds->members.size(); ++mi )
    {
	DataDef *mtype = dds->members[mi].second;
	size_t count = (mi < dds->member_counts.size()) ? dds->member_counts[mi] : 1;
	if ( mtype && mtype->basetype() == BaseType::btStruct )
	{
	    DataDefSTRUCT *nested = dynamic_cast<DataDefSTRUCT *>(mtype);
	    total += count_struct_init_slots(nested);
	}
	else
	    total += (count > 0) ? count : 1;
    }
    return total;
}

static bool is_compound_literal_value(TokenBase *node)
{
    TokenStructLit *slit = dynamic_cast<TokenStructLit *>(node);
    return slit && slit->datadef() && slit->datadef()->basetype() == BaseType::btStruct;
}

static size_t count_initializer_entries(const std::vector<TokenBase *> &inits)
{
    size_t total = 0;
    for ( TokenBase *node : inits )
    {
	if ( !node )
	    continue;
	if ( is_compound_literal_value(node) )
	{
	    ++total;
	    continue;
	}
	if ( dynamic_cast<TokenStructLit *>(node) != NULL )
	{
	    ++total;
	    continue;
	}
	++total;
    }
    return total;
}

void emit_zero_fill_region(Program &pgm, x86::Gp &base_reg, int32_t base_ofs, size_t total)
{
    size_t ofs = 0;
    for ( ; ofs + 8 <= total; ofs += 8 )
	pgm.cc.mov(x86::qword_ptr(base_reg, base_ofs + (int32_t)ofs), imm(0));
    for ( ; ofs + 4 <= total; ofs += 4 )
	pgm.cc.mov(x86::dword_ptr(base_reg, base_ofs + (int32_t)ofs), imm(0));
    for ( ; ofs < total; ++ofs )
	pgm.cc.mov(x86::byte_ptr(base_reg, base_ofs + (int32_t)ofs), imm(0));
}

static bool aggregate_has_runtime_size(DataDef *copy_type)
{
    DataDefCArray *add = dynamic_cast<DataDefCArray *>(copy_type);
    if ( add )
	return add->has_runtime_size()
	    || aggregate_has_runtime_size(add->element_type);
    DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(copy_type);
    return sdd && sdd->has_runtime_size();
}

x86::Gp emit_runtime_struct_size(Program &pgm, DataDefSTRUCT *sdd, const char *name)
{
    if ( !sdd )
	throw "emit_runtime_struct_size: missing struct type";
    x86::Gp total = pgm.cc.newGpq("%s", name ? name : "runtime_struct_size");
    pgm.cc.mov(total, imm((int64_t)sdd->size));
    for ( size_t i = 0; i < sdd->members.size(); ++i )
    {
	TokenBase *count_expr = (i < sdd->member_count_exprs.size()) ? sdd->member_count_exprs[i] : NULL;
	if ( !count_expr )
	    continue;
	DataDef *member_type = sdd->members[i].second;
	size_t fixed_mult = (i < sdd->member_counts.size()) ? sdd->member_counts[i] : 1;
	size_t elem_bytes = member_type ? member_type->size * fixed_mult : 0;
	regdefp_t count_rdp = {NULL, NULL, NULL};
	Operand &count_op = count_expr->compile(pgm, count_rdp);
	x86::Gp count_gp = pgm.cc.newGpq("runtime_member_count");
	load_idx_to_gpq(pgm, count_gp, count_op);
	if ( elem_bytes > 1 )
	    pgm.cc.imul(count_gp, count_gp, imm((int64_t)elem_bytes));
	pgm.cc.add(total, count_gp);
    }
    return total;
}

static x86::Gp emit_runtime_aggregate_size(Program &pgm, DataDef *copy_type, const char *name)
{
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(copy_type) )
    {
	x86::Gp total = aggregate_has_runtime_size(add->element_type)
	    ? emit_runtime_aggregate_size(pgm, add->element_type, "runtime_array_elem")
	    : pgm.cc.newGpq("%s", name ? name : "runtime_array_size");
	if ( !aggregate_has_runtime_size(add->element_type) )
	    pgm.cc.mov(total, imm((int64_t)(add->element_type ? add->element_type->size : 0)));
	if ( add->count_expr )
	{
	    regdefp_t count_rdp = {NULL, NULL, NULL};
	    Operand &count_op = add->count_expr->compile(pgm, count_rdp);
	    x86::Gp count_gp = pgm.cc.newGpq("runtime_array_count");
	    load_idx_to_gpq(pgm, count_gp, count_op);
	    pgm.cc.imul(total, count_gp);
	}
	else if ( add->count != 1 )
	    pgm.cc.imul(total, total, imm((int64_t)add->count));
	return total;
    }
    if ( DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(copy_type) )
	return emit_runtime_struct_size(pgm, sdd, name);
    x86::Gp total = pgm.cc.newGpq("%s", name ? name : "runtime_aggregate_size");
    pgm.cc.mov(total, imm((int64_t)(copy_type ? copy_type->size : 0)));
    return total;
}

static void emit_runtime_aggregate_copy(Program &pgm, Operand &dst, Operand &src,
					x86::Gp &size_gp, const char *name)
{
    x86::Gp dst_gp = as_gp_ptr(pgm, dst, name ? name : "aggregate_copy_dst");
    x86::Gp src_gp = as_gp_ptr(pgm, src, name ? name : "aggregate_copy_src");
    InvokeNode *call;
    pgm.cc.invoke(&call, imm((void *)madc_runtime_memcpy),
	FuncSignature::build<void *, void *, const void *, size_t>());
    call->setArg(0, dst_gp);
    call->setArg(1, src_gp);
    call->setArg(2, size_gp);
}

static size_t internal_vararg_static_slot_size(DataDef *type)
{
    if ( !type )
	return 8;
    if ( aggregate_has_runtime_size(type) )
	return 8;
    if ( type->is_numeric() || type->is_pointer() || type->is_real() )
	return 8;
    if ( type->size > 0 )
	return (size_t)type->size;
    return 8;
}

bool is_large_struct_return(DataDef *ret_type)
{
    if ( !ret_type || ret_type->size <= 16 )
	return false;
    if ( ret_type->basetype() == BaseType::btStruct )
	return true;
    // Wide SIMD (>16 bytes): can't fit in xmm0, needs hidden __retbuf
    // like large structs.
    if ( ret_type->is_simd() )
	return true;
    return false;
}

void emit_small_struct_return(Program &pgm, Operand &src, DataDef *ret_type)
{
    if ( !ret_type || ret_type->basetype() != BaseType::btStruct )
	throw "emit_small_struct_return() requires struct return type";
    if ( ret_type->size <= 0 || ret_type->size > 16 )
	throw "emit_small_struct_return() requires 1..16 byte struct";

    x86::Gp base_ptr = x86::rcx;
    if ( src.isMem() )
	pgm.cc.lea(base_ptr, src.as<x86::Mem>());
    else if ( src.isReg() && src.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	x86::Gp src_gp = src.as<x86::Gp>();
	if ( src_gp.id() != base_ptr.id() )
	    pgm.cc.mov(base_ptr, src_gp);
    }
    else
	throw "emit_small_struct_return() unsupported source operand";

    x86::Mem lo = x86::qword_ptr(base_ptr);
    lo.setSize(8);
    pgm.cc.mov(x86::rax, lo);

    if ( ret_type->size > 8 )
    {
	x86::Mem hi = x86::qword_ptr(base_ptr, 8);
	hi.setSize(8);
	pgm.cc.mov(x86::rdx, hi);
    }

    pgm.cc.ret(x86::rax);
}

bool token_has_constant_cstring(TokenBase *token)
{
    if ( !token )
	return false;
    if ( token->type() == TokenType::ttString )
	return true;
    TokenVar *tv = dynamic_cast<TokenVar *>(token);
    return tv
	&& tv->var.is_constant()
	&& tv->var.type
	&& tv->var.type->is_string()
	&& tv->var.data;
}

static bool token_get_constant_cstring(TokenBase *token, std::string &out)
{
    if ( !token )
	return false;
    if ( token->type() == TokenType::ttString )
    {
	out = static_cast<TokenStr *>(token)->str;
	return true;
    }
    TokenVar *tv = dynamic_cast<TokenVar *>(token);
    if ( tv
      && tv->var.is_constant()
      && tv->var.type
      && tv->var.type->is_string()
      && tv->var.data )
    {
	out = *((std::string *)tv->var.data);
	return true;
    }
    return false;
}

int64_t estimate_constant_printf_output(TokenBase *fmt_token,
					       const std::vector<TokenBase *> &args,
					       size_t arg_index)
{
    std::string fmt;
    if ( !token_get_constant_cstring(fmt_token, fmt) )
	return -1;

    int64_t total = 0;
    for ( size_t i = 0; i < fmt.size(); ++i )
    {
	if ( fmt[i] != '%' )
	{
	    ++total;
	    continue;
	}
	if ( i + 1 < fmt.size() && fmt[i + 1] == '%' )
	{
	    ++total;
	    ++i;
	    continue;
	}

	size_t spec = i + 1;
	while ( spec < fmt.size() && strchr("-+ #0", fmt[spec]) )
	    ++spec;
	while ( spec < fmt.size() && isdigit((unsigned char)fmt[spec]) )
	    ++spec;
	if ( spec < fmt.size() && fmt[spec] == '.' )
	{
	    ++spec;
	    while ( spec < fmt.size() && isdigit((unsigned char)fmt[spec]) )
		++spec;
	}
	while ( spec < fmt.size() && strchr("hlLzjt", fmt[spec]) )
	    ++spec;
	if ( spec >= fmt.size() || arg_index >= args.size() )
	    return -1;

	char conv = fmt[spec];
	if ( conv == 's' )
	{
	    std::string s;
	    if ( !token_get_constant_cstring(args[arg_index++], s) )
		return -1;
	    total += (int64_t)s.size();
	}
	else if ( conv == 'c' )
	{
	    int64_t ch = 0;
	    if ( !try_eval_const_i64(args[arg_index++], ch) )
		return -1;
	    (void)ch;
	    ++total;
	}
	else if ( conv == 'd' || conv == 'i' )
	{
	    int64_t val = 0;
	    if ( !try_eval_const_i64(args[arg_index++], val) )
		return -1;
	    total += (int64_t)std::to_string(val).size();
	}
	else if ( conv == 'u' )
	{
	    int64_t val = 0;
	    if ( !try_eval_const_i64(args[arg_index++], val) )
		return -1;
	    total += (int64_t)std::to_string((uint64_t)val).size();
	}
	else
	    return -1;

	i = spec;
    }

    return total;
}

static bool token_get_constant_cstring_address(TokenBase *token, const char *&out)
{
    out = NULL;
    if ( !token )
	return false;

    const char *cstr = token_constant_cstring(token);
    if ( cstr )
    {
	out = cstr;
	return true;
    }

    if ( TokenCast *tc = dynamic_cast<TokenCast *>(token) )
	return token_get_constant_cstring_address(tc->expr, out);

    auto index_into_cstring = [&](const char *base_cstr, TokenBase *index) -> bool {
	if ( !base_cstr || !index || !index->is_constant() )
	    return false;
	int64_t idx = index->ival();
	if ( idx < 0 )
	    return false;
	size_t len = strlen(base_cstr);
	if ( static_cast<size_t>(idx) > len )
	    return false;
	out = base_cstr + idx;
	return true;
    };

    TokenAddrExpr *tae = dynamic_cast<TokenAddrExpr *>(token);
    if ( !tae || !tae->expr )
	return false;

    if ( TokenSubscript *ts = dynamic_cast<TokenSubscript *>(tae->expr) )
    {
	if ( !ts->object.type || !ts->object.type->is_string() || !ts->object.data )
	    return false;
	std::string *str = static_cast<std::string *>(ts->object.data);
	return index_into_cstring(str->c_str(), ts->index);
    }

    if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(tae->expr) )
	return index_into_cstring(token_constant_cstring(tse->base_expr), tse->index);

    return false;
}

bool type_is_cstr_pointer(DataDef *dd)
{
    return dd
	&& dd->is_pointer()
	&& (dd->rawtype() == DataType::dtCHAR
	 || dd->rawtype() == DataType::dtUINT8);
}

static bool type_is_cstr_element(DataDef *dd)
{
    return dd
	&& (dd->rawtype() == DataType::dtCHAR
	 || dd->rawtype() == DataType::dtUINT8);
}

static bool token_is_charptr_expr(TokenBase *token)
{
    if ( !token )
	return false;
    if ( token_has_constant_cstring(token) )
	return true;
    DataDef *dd = token->datadef();
    if ( dd )
    {
	if ( dd->is_string() )
	    return true;
	if ( type_is_cstr_pointer(dd) )
	    return true;
	if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(dd) )
	    return add->element_type && type_is_cstr_element(add->element_type);
    }
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(token) )
    {
	if ( tv->var.is_fixed_array()
	  && tv->var.type
	  && type_is_cstr_element(tv->var.type) )
	    return true;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(token) )
    {
	if ( tm->is_fixed_array_member()
	  && tm->var.type
	  && type_is_cstr_element(tm->var.type) )
	    return true;
    }
    if ( TokenTerQ *tq = dynamic_cast<TokenTerQ *>(token) )
	return token_is_charptr_expr(tq->true_expr) && token_is_charptr_expr(tq->false_expr);
    return false;
}

static bool token_compiles_naturally_as_charptr(TokenBase *token)
{
    if ( !token )
	return false;
    DataDef *dd = token->datadef();
    if ( type_is_cstr_pointer(dd) )
	return true;
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(dd) )
	return add->element_type && type_is_cstr_element(add->element_type);
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(token) )
	return tv->var.is_fixed_array()
	    && tv->var.type
	    && type_is_cstr_element(tv->var.type);
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(token) )
	return tm->is_fixed_array_member()
	    && tm->var.type
	    && type_is_cstr_element(tm->var.type);
    if ( TokenTerQ *tq = dynamic_cast<TokenTerQ *>(token) )
	return token_is_charptr_expr(tq->true_expr)
	    && token_is_charptr_expr(tq->false_expr);
    return false;
}

void emit_zeroed_void_return(Program &pgm)
{
    pgm.cc.xor_(x86::eax, x86::eax);
    pgm.cc.ret();
}

static bool should_instrument_function(Program &pgm, FuncDef *func)
{
    if ( !pgm.instrument_functions || !func || func->no_instrument_function )
	return false;
    return pgm.cur_func_name != "__cyg_profile_func_enter"
	&& pgm.cur_func_name != "__cyg_profile_func_exit";
}

static bool resolve_function_address(Program &pgm, Variable *var, x86::Gp &addr_gp)
{
    if ( !var || !var->data )
	return false;

    Method *method = (Method *)var->data;
    FuncDef *func = dynamic_cast<FuncDef *>(method->returns.type);
    if ( func && func->funcnode )
    {
	pgm.cc.lea(addr_gp, x86::ptr(func->funcnode->label()));
	return true;
    }

    void *func_addr = method->x86code;
    if ( !func_addr && pgm.is_dynamic_symbol_allowed(var->name) )
    {
	func_addr = dlsym(RTLD_DEFAULT, var->name.c_str());
	if ( func_addr )
	    method->x86code = func_addr;
    }
    if ( !func_addr )
	return false;

    pgm.cc.mov(addr_gp, imm((uint64_t)func_addr));
    return true;
}

static void emit_function_instrument_hook(Program &pgm, FuncDef *current_func,
					  const char *hook_name)
{
    if ( !should_instrument_function(pgm, current_func)
      || !pgm.tkProgram || !hook_name )
	return;

    std::string hook(hook_name);
    Variable *hook_var = pgm.tkProgram->findVariable(hook);
    if ( !hook_var || !hook_var->data )
	return;

    x86::Gp hook_gp = pgm.cc.newIntPtr("%s", hook_name);
    if ( !resolve_function_address(pgm, hook_var, hook_gp) )
	return;

    x86::Gp current_gp = pgm.cc.newIntPtr("instrument_this_fn");
    if ( current_func->funcnode )
	pgm.cc.lea(current_gp, x86::ptr(current_func->funcnode->label()));
    else
	return;

    x86::Gp parent_gp = pgm.cc.newIntPtr("instrument_parent_fn");
    pgm.cc.xor_(parent_gp, parent_gp);

    InvokeNode *call;
    pgm.cc.invoke(&call, hook_gp,
	FuncSignature::build<void, void *, void *>());
    call->setArg(0, current_gp);
    call->setArg(1, parent_gp);
}

static void emit_function_instrument_enter(Program &pgm, FuncDef *current_func)
{
    emit_function_instrument_hook(pgm, current_func, "__cyg_profile_func_enter");
}

void emit_function_instrument_exit(Program &pgm, FuncDef *current_func)
{
    emit_function_instrument_hook(pgm, current_func, "__cyg_profile_func_exit");
}

static bool global_has_compilable_address(Program &pgm, Variable *var)
{
    if ( !var || !var->is_global() )
	return false;
    if ( var->data || var->has_aot_data() )
	return true;
    if ( pgm.tkProgram )
    {
	Variable *alias_target = pgm.resolve_global_storage_variable(var);
	if ( alias_target && alias_target != var
	  && (alias_target->data || alias_target->has_aot_data()) )
	{
	    if ( !var->data )
		var->data = alias_target->data;
	    var->flags &= ~vfSTACK;
	    if ( !var->has_aot_data() && alias_target->has_aot_data() )
	    {
		var->aot_data_offset = alias_target->aot_data_offset;
		var->aot_cstr_offset = alias_target->aot_cstr_offset;
	    }
	    return true;
	}
	Variable *canon = pgm.tkProgram->findVariable(var->name);
	if ( canon && (canon->data || canon->has_aot_data()) )
	{
	    if ( !var->data )
		var->data = canon->data;
	    if ( !var->has_aot_data() && canon->has_aot_data() )
	    {
		var->aot_data_offset = canon->aot_data_offset;
		var->aot_cstr_offset = canon->aot_cstr_offset;
	    }
	    return true;
	}
    }
    return false;
}

static Variable *canonical_scope_variable(TokenCpnd *scope, Variable *var)
{
    if ( !scope || !var )
	return var;

    if ( !(var->flags & vfSTACK) && !(var->flags & vfPARAM) )
	return var;

    Variable *found = scope->findVariable(var->name);
    if ( found && found != var && found->type == var->type )
	return found;

    if ( scope->method )
    {
	found = scope->method->findParameter(var->name);
	if ( found && found != var && found->type == var->type )
	    return found;
    }

    return var;
}

static const char *token_constant_cstring(TokenBase *token);

static bool global_string_initializer_is_static_data(Program &pgm, Variable &var, TokenBase *initialize)
{
    if ( !initialize || pgm.tkFunction != pgm.tkProgram )
	return false;
    if ( !var.is_global() || !var.type || !var.type->is_string() )
	return false;

    TokenAssign *assign = dynamic_cast<TokenAssign *>(initialize);
    if ( !assign || !assign->right )
	return false;

    const char *cstr = token_constant_cstring(assign->right);
    if ( !cstr )
	return false;

    if ( var.data )
	*static_cast<std::string *>(var.data) = cstr;

    return true;
}

static const char *token_constant_cstring(TokenBase *token)
{
    if ( !token )
	return NULL;
    if ( token->type() == TokenType::ttString )
	return static_cast<TokenStr *>(token)->str.c_str();
    TokenVar *tv = dynamic_cast<TokenVar *>(token);
    if ( tv
      && tv->var.is_constant()
      && tv->var.type
      && tv->var.type->is_string()
      && tv->var.data )
	return static_cast<std::string *>(tv->var.data)->c_str();
    return NULL;
}

static bool emit_constant_cstring_ptr(Program &pgm, TokenBase *token, x86::Gp &out)
{
    const char *cstr = token_constant_cstring(token);
    if ( !cstr )
	return false;
    if ( pgm.aot_tracking )
    {
	size_t len = strlen(cstr);
	char *buf = new char[len + 1];
	memcpy(buf, cstr, len + 1);
	pgm.aot_string_constants.push_back(buf);
	pgm.emit_data_mov(out, buf);
    }
    else
	pgm.cc.mov(out, imm((uint64_t)cstr));
    return true;
}

static std::string external_symbol_export_name(const std::string &requested_name, void *sym)
{
    Dl_info dli;
    if ( dladdr(sym, &dli) && dli.dli_sname && dli.dli_sname[0] )
    {
	std::string actual_name = dli.dli_sname;
	if ( actual_name.find("__vdso_") == 0 )
	    return requested_name;
	return actual_name;
    }
    return requested_name;
}

// Allocate an Xmm with a scalar-real type hint (kFloat32x1 or
// kFloat64x1) so asmjit's Compiler register allocator treats it
// correctly as a scalar float / double rather than the default
// `kInt32x4` (4-element int vector). Mismatched type hints make
// the allocator's liveness path interleave float and int-vector
// uses, producing filename-length-dependent reordering of varargs
// xmm setup. Pass `dd` as the float / double type, or NULL to
// default to double.
x86::Xmm newScalarXmm(Program &pgm, DataDef *dd, const char *name)
{
    if ( dd && dd->size == sizeof(float) )
	return pgm.cc.newXmmSs("%s", name ? name : "_xmm_ss");
    return pgm.cc.newXmmSd("%s", name ? name : "_xmm_sd");
}

// Whether a dlsym-resolved libc/system call returns a plain 32-bit int
// (vs an int64 / pointer / real). True for sub-int64 return types and for
// a curated whitelist of widely-used libc functions whose declared return
// is `int` even though the parser usually registers them as int64. The
// caller uses this to insert a movsxd sign-extension on the AX result.
static bool is_int32_dlsym_ret(DataDef *ret_type, const std::string &fname)
{
    static const std::set<std::string> int32_returners = {
	// comparison / search
	"strcmp", "strncmp", "strcasecmp", "strncasecmp",
	"memcmp", "strcoll",
	// char-level I/O
	"getchar", "putchar", "getc", "putc", "fgetc", "fputc", "ungetc",
	// stdio returning # written / status / 0
	"printf", "fprintf", "sprintf", "snprintf", "dprintf",
	"scanf", "fscanf", "sscanf", "vprintf", "vfprintf",
	"vsprintf", "vsnprintf", "vscanf", "vfscanf", "vsscanf",
	"fputs", "puts", "fflush", "fclose", "fileno", "feof",
	"ferror", "setvbuf", "setbuf",
	// file / process status
	"remove", "rename", "unlink", "rmdir", "mkdir", "chmod",
	"chown", "access", "link", "symlink", "truncate",
	"ftruncate", "fcntl", "ioctl", "dup", "dup2", "close",
	"open", "creat", "fsync", "pipe",
	"stat", "fstat", "lstat", "fstatat", "statfs", "fstatfs",
	"utime", "utimes", "futimes",
	// process / signal
	"kill", "fork", "exec", "execv", "execvp", "execve",
	"wait", "waitpid", "getpid", "getppid", "getuid", "geteuid",
	"getgid", "getegid", "setuid", "setgid", "system",
	"atexit", "raise", "sigaction", "sigprocmask",
	// conversions
	"atoi", "atol",
	// network / socket
	"socket", "bind", "listen", "accept", "connect", "send",
	"recv", "sendto", "recvfrom", "shutdown", "setsockopt",
	"getsockopt", "getsockname", "getpeername", "inet_pton",
	"inet_aton", "select", "poll", "flock",
	// time
	"gettimeofday", "settimeofday", "clock_gettime",
	"clock_settime", "nanosleep", "sleep", "usleep",
	// string helpers that happen to return int
	"strerror_r", "strtol_safe"
    };
    if ( ret_type && !ret_type->is_pointer() && !ret_type->is_real()
	 && ret_type->is_integer() && ret_type->size < 8 )
	return true;
    return int32_returners.count(fname) > 0;
}

IRValue ir_from_operand(const Operand &op, DataDef *type)
{
    if ( !type )
	throw "ir_from_operand() type is NULL";
    if ( op.isReg() )
	return IRValue::reg(op, type);
    if ( op.isMem() )
	return IRValue::mem(op.as<x86::Mem>(), type);
    if ( op.isImm() )
	return IRValue::imm(op.as<Imm>(), type);
    return IRValue();
}

Operand &emit_ir_value(Program &pgm, IRValue value, regdefp_t &regdp,
		       Operand &storage, DataDef *fallback_type)
{
    auto materialize_simd = [&](IRValue src, DataDef *dst_type, x86::Xmm &dst) {
	DataDef *src_type = src.type ? src.type : dst_type;
	if ( src.isReg() && src.op.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    pgm.safemov(dst, src.op.as<x86::Xmm>(), dst_type, src_type);
	    return;
	}
	if ( src.isMem() )
	{
	    load_mem_to_xmm(pgm, dst, src.op.as<x86::Mem>(), src_type);
	    return;
	}
	if ( src.isReg() && src.op.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    if ( dst_type && dst_type->size <= 8 )
		pgm.cc.movq(dst, src.op.as<x86::Gp>());
	    else
		throw "emit_ir_value() SIMD materialization cannot widen gp source beyond 64 bits";
	    return;
	}
	if ( src.isImm() )
	{
	    x86::Gp gp = pgm.cc.newGpq("ir_simd_imm");
	    pgm.cc.mov(gp, src.op.as<Imm>());
	    pgm.cc.movq(dst, gp);
	    return;
	}
	throw "emit_ir_value() unsupported SIMD source";
    };

    if ( regdp.second && regdp.second->is_simd() )
    {
	x86::Xmm xmm = regdp.second->newreg(pgm.cc, "ir_simd").as<x86::Xmm>();
	materialize_simd(value, regdp.second, xmm);
	if ( regdp.first )
	{
	    if ( regdp.first->isMem() )
	    {
		x86::Mem dst = regdp.first->as<x86::Mem>();
		store_xmm_to_mem(pgm, dst, xmm, regdp.second);
		return *regdp.first;
	    }
	    if ( regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec) )
	    {
		pgm.safemov(*regdp.first, xmm, regdp.second, regdp.second);
		return *regdp.first;
	    }
	}
	storage = xmm;
	regdp.first = &storage;
	return storage;
    }

    IRBuilder ir(pgm.cc);
    if ( !regdp.second )
	regdp.second = fallback_type ? fallback_type : value.type;
    if ( regdp.first )
    {
	if ( regdp.first->isMem() )
	{
	    ir.store(IRValue::mem(regdp.first->as<x86::Mem>(), regdp.second), value);
	    return *regdp.first;
	}
	if ( regdp.first->isReg() )
	{
	    IRValue out = ir.coerce(value, regdp.second);
	    out = ir.load(out);
	    if ( !out.op.equals(*regdp.first) )
		pgm.safemov(*regdp.first, out.op, regdp.second, out.type);
	    return *regdp.first;
	}
    }

    IRValue out = ir.coerce(value, regdp.second);
    out = ir.load(out);
    storage = out.op;
    regdp.first = &storage;
    return storage;
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









DataDef *promote_c_integer_type(DataDef *type)
{
    if ( !type || !type->is_integer() )
	return type;
    if ( type->size < 4 )
	return &ddINT32;
    return type;
}




DataDef *infer_numeric_type(TokenBase *left, TokenBase *right)
{
    DataDef *lt = token_numeric_type(left);
    DataDef *rt = token_numeric_type(right);
    // C usual arithmetic conversions for floating types:
    // pick the highest-ranked real type present.  If only one side
    // is real, the integer side converts to that real type — NOT
    // unconditionally to double.
    bool l_real = (lt && lt->is_real()) || (left && left->is_real());
    bool r_real = (rt && rt->is_real()) || (right && right->is_real());
    if ( l_real || r_real )
    {
	DataDef *ld = lt && lt->is_real() ? lt : (left ? left->datadef() : nullptr);
	DataDef *rd = rt && rt->is_real() ? rt : (right ? right->datadef() : nullptr);
	bool l_is_real = ld && ld->is_real();
	bool r_is_real = rd && rd->is_real();
	if ( l_is_real && r_is_real )
	    return ld->size >= rd->size ? ld : rd;
	if ( l_is_real )
	    return ld;
	if ( r_is_real )
	    return rd;
	return &ddDOUBLE;  // fallback
    }
    if ( lt && lt->is_integer() && rt && rt->is_integer() )
	return usual_binary_integer_type(left, lt, right, rt);
    // Unary / single-operand case: preserve the exact type so callers
    // like TokenBnot can mask correctly.
    if ( lt && lt->is_integer() )
	return choose_integer_type(left, lt, right, NULL);
    if ( rt && rt->is_integer() )
	return choose_integer_type(left, NULL, right, rt);
    return &ddINT;
}





Operand &compile_token_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
					 Operand *preferred_dest, Operand &storage)
{
    bool decay_token = false;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(token) )
	if ( tv->var.is_fixed_array() )
	    decay_token = true;
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(token) )
	if ( tm->is_fixed_array_member() )
	    decay_token = true;
    if ( token && dynamic_cast<DataDefCArray *>(token->datadef()) )
	decay_token = true;

    bool target_is_charptr = target_type && target_type->is_pointer()
	&& (target_type->rawtype() == DataType::dtCHAR
	 || target_type->rawtype() == DataType::dtUINT8);
    if ( target_is_charptr && token_has_constant_cstring(token) )
    {
	x86::Gp cstr = pgm.cc.newIntPtr("norm_cstr");
	emit_constant_cstring_ptr(pgm, token, cstr);
	if ( preferred_dest && preferred_dest->isReg()
	  && preferred_dest->as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    if ( !cstr.equals(*preferred_dest) )
		pgm.safemov(*preferred_dest, cstr, target_type, target_type);
	    storage = *preferred_dest;
	    return storage;
	}
	storage = cstr;
	return storage;
    }

    DataDef *effective_ptr_type = effective_pointer_type_for_arith(pgm, token);

    if ( target_type && token && token->datadef()
      && token->datadef()->is_complex()
      && !effective_ptr_type
      && !target_type->is_complex()
      && (target_type->is_numeric() || target_type->is_pointer()) )
    {
	TokenComplexPart real_part(token, false);
	regdefp_t inner_rdp = {nullptr, complex_component_type(token->datadef()), nullptr};
	Operand &real_op = real_part.compile(pgm, inner_rdp);
	DataDef *real_type = inner_rdp.second ? inner_rdp.second : real_part.datadef();
	IRBuilder ir(pgm.cc);
	IRValue out = ir.coerce(ir_from_operand(real_op, real_type), target_type);
	out = ir.load(out);
	if ( preferred_dest && preferred_dest->isReg() )
	{
	    if ( !out.op.equals(*preferred_dest) )
		pgm.safemov(*preferred_dest, out.op, target_type, out.type);
	    storage = *preferred_dest;
	    return storage;
	}
	storage = out.op;
	return storage;
    }

    regdefp_t tmp_rdp = {preferred_dest, target_type, nullptr};
    if ( target_type && target_type->is_simd()
      && token && token->datadef() && !token->datadef()->is_simd() )
    {
	tmp_rdp.first = nullptr;
	tmp_rdp.second = nullptr;
    }
    // Arithmetic / bitwise operators must compute at their natural
    // operand type, not the caller's target.
    //
    // Case 1: caller's wider signed target (e.g. int64 from a comparison)
    // overrides the operator's unsigned narrower type (e.g. uint32 from
    // `0U ^ (short)-0x8000`), causing wrong sign-extension.
    //
    // Case 2: bitwise operators (&, |, ^, <<, >>) always produce integer
    // results, even when the caller wants a double. Passing a double
    // target to a bitwise op makes it try to operate in XMM mode, which
    // is wrong. Let it compute as integer, then coerce below.
    if ( is_arithmetic_result_operator(token) )
    {
	TokenOperator *op = dynamic_cast<TokenOperator *>(token);
	bool is_bitwise = op && (op->id() == TokenID::tkBand
	    || op->id() == TokenID::tkBor || op->id() == TokenID::tkXor
	    || op->id() == TokenID::tkBSL || op->id() == TokenID::tkBSR);
	if ( is_bitwise && target_type && target_type->is_real() )
	{
	    // Bitwise ops are always integer — let them infer their own type
	    tmp_rdp.second = nullptr;
	    tmp_rdp.first = nullptr;
	}
	else if ( target_type && target_type->is_integer() && op )
	{
	    DataDef *natural = infer_numeric_type(op->left, op->right);
	    if ( natural && natural->is_integer() && natural->is_unsigned()
	      && !target_type->is_unsigned() )
	    {
		tmp_rdp.second = nullptr;
		tmp_rdp.first = nullptr;
	    }
	    // Unsigned operands narrower than target: C requires arithmetic
	    // at operand width. 0U - 1U must wrap to 0xFFFFFFFF (32-bit),
	    // not 0xFFFFFFFFFFFFFFFF (64-bit). Coerce widens afterward.
	    else if ( natural && natural->is_integer() && natural->is_unsigned()
	      && target_type->is_unsigned() && natural->size < target_type->size )
	    {
		tmp_rdp.second = nullptr;
		tmp_rdp.first = nullptr;
	    }
	}
    }
    // Unary minus on unsigned operand narrower than target: -2U must
    // negate at 32-bit (0xFFFFFFFE) not 64-bit (0xFFFFFFFFFFFFFFFE).
    // C defines the result type as the operand type; the coerce step
    // below handles zero-extension to the wider target.
    if ( token && token->id() == TokenID::tkNeg
      && target_type && target_type->is_integer() )
    {
	TokenNeg *neg = dynamic_cast<TokenNeg *>(token);
	if ( neg && neg->right && neg->right->datadef()
	  && neg->right->datadef()->is_unsigned()
	  && neg->right->datadef()->size < target_type->size )
	{
	    tmp_rdp.second = nullptr;
	    tmp_rdp.first = nullptr;
	}
    }
    if ( target_type
      && target_type->is_pointer()
      && ((target_is_charptr && token_has_constant_cstring(token))
       || decay_token) )
	tmp_rdp.second = nullptr;
    // String→charptr coercion for non-constant strings: let the token
    // compile with its natural dtSTRING type so ir.coerce can call
    // string_cstr. But keep true char*-flavored expressions (notably
    // ternaries of string literals / char* branches) on the requested
    // char* target so they lower like GCC: raw pointers end to end,
    // no transient std::string merge object. That avoids native EXE
    // paths reloading the first machine word of a copied string object
    // before the eventual c_str() call.
    if ( target_is_charptr
      && token && token->datadef()
      && token->datadef()->is_string()
      && !token_has_constant_cstring(token)
      && !token_compiles_naturally_as_charptr(token) )
	tmp_rdp.second = nullptr;
    Operand &raw = token->compile(pgm, tmp_rdp);
    DataDef *raw_type = tmp_rdp.second ? tmp_rdp.second : (token && token->datadef() ? token->datadef() : target_type);
    if ( target_type && target_type->is_pointer() )
    {
	if ( decay_token && raw_type && !raw_type->is_pointer() )
	    raw_type = effective_ptr_type ? effective_ptr_type : &ddLPSTR;
    }
    if ( target_type && target_type->is_simd() )
    {
	// Wide SIMD (>16 bytes): can't fit in Xmm.  If the raw result is
	// already Mem-backed (compound literal buffer, variable), return it
	// directly.  Otherwise copy to a stack buffer.
	if ( target_type->size > 16 )
	{
	    if ( raw.isMem() )
	    {
		if ( preferred_dest && preferred_dest->isMem() )
		{
		    Operand dst_copy = *preferred_dest;
		    emit_raw_aggregate_copy(pgm, dst_copy, raw, target_type, "norm_wide_simd");
		    storage = *preferred_dest;
		    return storage;
		}
		storage = raw;
		return storage;
	    }
	    // Xmm or Gp result for >16 bytes shouldn't happen, but handle
	    // gracefully: spill to a stack buffer.
	    x86::Mem buf = pgm.cc.newStack((uint32_t)target_type->size, 16);
	    if ( raw.isReg() && raw.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.cc.movups(buf, raw.as<x86::Xmm>());
	    else if ( raw.isReg() && raw.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.mov(buf, raw.as<x86::Gp>());
	    storage = buf;
	    return storage;
	}
	Operand simd_dst = preferred_dest ? *preferred_dest : target_type->newreg(pgm.cc, "norm_simd");
	if ( !simd_dst.isReg() || !simd_dst.as<BaseReg>().isGroup(RegGroup::kVec) )
	    throw "compile_token_normalized() SIMD target needs Xmm destination";
	x86::Xmm xmm = simd_dst.as<x86::Xmm>();
	if ( raw.isReg() && raw.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    // If source is a scalar Xmm (float/double) being promoted to SIMD,
	    // coerce to element type and broadcast to all lanes.
	    if ( raw_type && raw_type->is_real() && !raw_type->is_simd() )
	    {
		DataDefSIMD *vdd = static_cast<DataDefSIMD *>(target_type);
		DataDef *elem = vdd->element_type ? vdd->element_type : &ddDOUBLE;
		IRBuilder ir(pgm.cc);
		IRValue scalar = ir.coerce(ir_from_operand(raw, raw_type), elem);
		scalar = ir.load(scalar);
		splat_xmm_to_simd_xmm(pgm, xmm, scalar.op.as<x86::Xmm>(), vdd);
	    }
	    else
		pgm.safemov(xmm, raw.as<x86::Xmm>(), target_type, raw_type);
	}
	else if ( raw.isMem() )
	{
	    x86::Mem mem = raw.as<x86::Mem>();
	    load_mem_to_xmm(pgm, xmm, mem, raw_type ? raw_type : target_type);
	}
	else if ( raw.isReg() && raw.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    DataDefSIMD *vdd = static_cast<DataDefSIMD *>(target_type);
	    DataDef *elem = vdd->element_type ? vdd->element_type : &ddINT64;
	    IRBuilder ir(pgm.cc);
	    IRValue scalar = ir.coerce(ir_from_operand(raw, raw_type ? raw_type : elem), elem);
	    scalar = ir.load(scalar);
	    splat_scalar_to_simd(pgm, xmm, scalar, vdd);
	}
	else if ( raw.isImm() )
	{
	    DataDefSIMD *vdd = static_cast<DataDefSIMD *>(target_type);
	    DataDef *elem = vdd->element_type ? vdd->element_type : &ddINT64;
	    IRBuilder ir(pgm.cc);
	    IRValue scalar = ir.coerce(IRValue::imm(raw.as<Imm>(), raw_type ? raw_type : elem), elem);
	    scalar = ir.load(scalar);
	    splat_scalar_to_simd(pgm, xmm, scalar, vdd);
	}
	else
	    throw "compile_token_normalized() unsupported SIMD source operand";
	storage = simd_dst;
	return storage;
    }
    IRBuilder ir(pgm.cc);
    IRValue out = ir.coerce(ir_from_operand(raw, raw_type), target_type);
    out = ir.load(out);
    if ( preferred_dest && preferred_dest->isReg() )
    {
	if ( !out.op.equals(*preferred_dest) )
	    pgm.safemov(*preferred_dest, out.op, target_type, out.type);
	storage = *preferred_dest;
	return storage;
    }
    storage = out.op;
    return storage;
}


Operand &compile_token_gp_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
					    Operand &storage)
{
    Operand &out = compile_token_normalized(pgm, token, target_type, nullptr, storage);
    if ( !out.isReg() || !out.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "compile_token_gp_normalized() expected gp register";
    return out;
}


// Shared shape for the typesafe binary ops (safeadd / safesub /
// safemul): in-place on lval, producing lval := lval <op> rval. All
// three take a pair of DataDef slots (both default NULL) for
// per-operand type info; the emit_plain_binop3 helper only sets the
// left slot since both operands are pre-normalized to result_type.
// unpack paths that need to move a var's value into a scratch Gp before
// storing it elsewhere.
void load_var_to_gp(Program &pgm, Operand &src, asmjit::x86::Gp &dst_gp)
{
    if ( src.isReg() && src.as<asmjit::BaseReg>().isGroup(asmjit::RegGroup::kGp) )
	pgm.cc.mov(dst_gp, src.as<asmjit::x86::Gp>());
    else if ( src.isMem() )
	pgm.cc.mov(dst_gp, src.as<asmjit::x86::Mem>());
    else
	throw "load_var_to_gp() unsupported source operand";
}

// Load an index Operand into dst_gpq (a 64-bit Gp). Widens sub-word Gp
// sources via movsxd / movsx so a `mov gpq, gpw` (illegal x86) doesn't
// fall through to asmjit's encoder. Used by every subscript / compound-
// subscript path that expects to compute `[base + idx*scale]`.
void load_idx_to_gpq(Program &pgm, asmjit::x86::Gp &dst, asmjit::Operand &src)
{
    if ( src.isImm() )
    {
	pgm.cc.mov(dst, src.as<asmjit::Imm>());
	return;
    }
    if ( src.isMem() )
    {
	asmjit::x86::Mem m = src.as<asmjit::x86::Mem>();
	uint32_t msz = m.x86RmSize();
	if ( msz > 0 && msz < 4 )
	    pgm.cc.movsx(dst, m);            // 8/16→64 sign-extend
	else if ( msz == 4 )
	    pgm.cc.movsxd(dst, m);           // 32→64 sign-extend
	else
	    pgm.cc.mov(dst, m);
	return;
    }
    if ( !src.isReg() || !src.as<asmjit::BaseReg>().isGroup(asmjit::RegGroup::kGp) )
	throw "load_idx_to_gpq() unsupported source operand";
    asmjit::x86::Gp s = src.as<asmjit::x86::Gp>();
    uint32_t sz = s.x86RmSize();
    if ( sz == 8 )
	pgm.cc.mov(dst, s);
    else if ( sz == 4 )
	pgm.cc.movsxd(dst, s);          // 32→64 sign-extend
    else if ( sz == 2 || sz == 1 )
	pgm.cc.movsx(dst, s);           // 16/8→64 sign-extend
    else
	pgm.cc.mov(dst, s);
}

uint32_t scale_index_by_element_size(Program &pgm, asmjit::x86::Gp &idx_reg,
					    DataDef *elem_type, const char *name)
{
    size_t elem_size = elem_type && elem_type->size ? elem_type->size : 8;
    if ( aggregate_has_runtime_size(elem_type) )
    {
	x86::Gp stride = emit_runtime_aggregate_size(pgm, elem_type, name);
	pgm.cc.imul(idx_reg, stride);
	return 0;
    }
    if ( elem_size == 8 ) return 3;
    if ( elem_size == 4 ) return 2;
    if ( elem_size == 2 ) return 1;
    if ( elem_size != 1 )
	pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)elem_size));
    return 0;
}

// Helper: load the address-of a variable into `dst_gp` — mov from Gp
// (a Gp already holds a pointer/address), lea from Mem (stack-backed
// var where we want its slot address). Used for non-numeric lambda
// captures where the env slot stores a pointer to the outer string.
void lea_var_to_gp(Program &pgm, Operand &src, asmjit::x86::Gp &dst_gp)
{
    if ( src.isReg() && src.as<asmjit::BaseReg>().isGroup(asmjit::RegGroup::kGp) )
	pgm.cc.mov(dst_gp, src.as<asmjit::x86::Gp>());
    else if ( src.isMem() )
	pgm.cc.lea(dst_gp, src.as<asmjit::x86::Mem>());
    else
	throw "lea_var_to_gp() unsupported source operand";
}

// Helper: store `src_gp` into a variable's operand (Reg or Mem) via
// the size-aware move. Used by multi-return unpack + lambda-capture
// reload paths.
void store_gp_to_var(Program &pgm, asmjit::x86::Gp &src_gp, Operand &dst)
{
    if ( dst.isReg() && dst.as<asmjit::BaseReg>().isGroup(asmjit::RegGroup::kGp) )
	pgm.cc.mov(dst.as<asmjit::x86::Gp>(), src_gp);
    else if ( dst.isMem() )
	pgm.cc.mov(dst.as<asmjit::x86::Mem>(), src_gp);
    else
	throw "store_gp_to_var() unsupported destination operand";
}

// State carried across the general-fallback scaffolding of the binary-op
// tokens (TokenAdd, TokenSub, TokenMul, TokenXor, TokenBand, TokenBor,
// TokenBSL general integer path). Captured at begin_general_binop,
// consumed at finish_general_binop.


// C pointer arithmetic: if `left` is a pointer or fixed-array type and
// `right` is a non-pointer integer offset, scale the offset by the
// pointed-to / element size so `p ± n` yields `p ± n*sizeof(*p)` bytes.
// No-op when `right` is also a pointer (pointer difference stays in
// raw bytes — an explicit `(long)p - (long)q` is how users opt in to
// the unscaled form anyway). Mutates `rval` in place. Safe to call
// from both TokenAdd and TokenSub; the previous inline copies diverged
// only in TokenSub's extra `right is not pointer` guard, which is now
// folded into the single helper.
// Convenience wrapper: dynamic_cast tb to TokenMember and call its
// is_fixed_array_member(). True for `SKILLTYPE *arr[N]`-style struct
// members where the storage is in-place but the member's datadef
// reports the element type.
bool is_fixed_array_struct_member(TokenBase *tb)
{
    TokenMember *tm = dynamic_cast<TokenMember *>(tb);
    return tm && tm->is_fixed_array_member();
}

bool subscript_object_uses_inplace_storage(const Variable &object)
{
    return object.is_fixed_array()
	|| (object.type && object.type->is_simd());
}

static DataDef *build_fixed_array_result_type(DataDef *base_type,
					      const std::vector<uint32_t> &dims,
					      size_t consumed_dims);

static const std::vector<uint32_t> *fixed_array_member_dims(TokenMember *tm)
{
    if ( !tm )
	return NULL;
    DataDefSTRUCT *sdd = tm->owner_struct_type();
    if ( !sdd )
	return NULL;
    return sdd->m_dims(tm->var.name);
}

DataDef *fixed_array_member_result_type(TokenMember *tm, size_t consumed_dims)
{
    if ( !tm || !tm->var.type )
	return &ddINT64;
    const std::vector<uint32_t> *dims = fixed_array_member_dims(tm);
    if ( !dims || dims->empty() )
	return tm->var.type;
    return build_fixed_array_result_type(tm->var.type, *dims, consumed_dims);
}

static DataDef *build_fixed_array_result_type(DataDef *base_type,
					      const std::vector<uint32_t> &dims,
					      size_t consumed_dims)
{
    if ( !base_type )
	return &ddINT64;
    if ( consumed_dims >= dims.size() )
	return base_type;

    DataDef *result = base_type;
    for ( size_t i = dims.size(); i-- > consumed_dims; )
	result = new DataDefCArray(*result, result->name, dims[i], NULL);
    return result;
}

DataDef *fixed_array_subscript_result_type(const Variable &object,
						  size_t consumed_dims)
{
    if ( !object.is_fixed_array() )
	return object.type ? object.type : &ddINT64;
    return build_fixed_array_result_type(object.type, object.dims, consumed_dims);
}

size_t fixed_array_subscript_stride(const Variable &object,
					   size_t consumed_dims)
{
    DataDef *result_type = fixed_array_subscript_result_type(object, consumed_dims);
    return (result_type && result_type->size) ? result_type->size : 8;
}












bool is_large_simd_type(DataDef *type)
{
    return type && type->is_simd() && type->size > 16;
}



x86::Mem simd_lane_mem(const x86::Mem &base, size_t lane, DataDefSIMD *vdd)
{
    DataDef *elem = vdd && vdd->element_type ? vdd->element_type : &ddINT64;
    size_t elem_size = elem->size ? elem->size : 8;
    x86::Mem out = base;
    out.addOffset((int64_t)(lane * elem_size));
    out.setSize((uint32_t)elem_size);
    return out;
}

x86::Mem new_large_simd_stack(Program &pgm, DataDefSIMD *vdd, const char *name)
{
    (void)name;
    uint32_t bytes = (uint32_t)(vdd ? vdd->size : 16);
    uint32_t align = (uint32_t)(vdd ? vdd->alignment() : 16);
    x86::Mem mem = pgm.cc.newStack(bytes, align ? align : 16);
    mem.setSize(bytes);
    return mem;
}


x86::Gp load_simd_lane_gp(Program &pgm, const x86::Mem &base, size_t lane,
			  DataDefSIMD *vdd, const char *name)
{
    DataDef *elem = vdd && vdd->element_type ? vdd->element_type : &ddINT64;
    x86::Gp gp = pgm.cc.newGpq("%s", name ? name : "simd_lane");
    x86::Mem mem = simd_lane_mem(base, lane, vdd);
    load_mem_to_gpq(pgm, gp, mem, elem);
    return gp;
}

void store_simd_lane_gp(Program &pgm, const x86::Mem &base, size_t lane,
			DataDefSIMD *vdd, x86::Gp value)
{
    DataDef *elem = vdd && vdd->element_type ? vdd->element_type : &ddINT64;
    x86::Mem mem = simd_lane_mem(base, lane, vdd);
    size_t size = elem && elem->size ? elem->size : 8;
    if ( size == 1 )
	pgm.cc.mov(mem, value.r8());
    else if ( size == 2 )
	pgm.cc.mov(mem, value.r16());
    else if ( size == 4 )
	pgm.cc.mov(mem, value.r32());
    else
	pgm.cc.mov(mem, value.r64());
}






void prepare_complex_component_pair(Program &pgm, TokenBase *expr, DataDef *target_complex_type,
					   Operand &base_storage,
					   Operand *&real_part, DataDef *&real_type,
					   Operand *&imag_part, DataDef *&imag_type,
					   Operand &real_storage, Operand &imag_storage)
{
    DataDefCOMPLEX *target_cdd = dynamic_cast<DataDefCOMPLEX *>(target_complex_type);
    if ( !target_cdd || !target_cdd->element_type )
	throw "prepare_complex_component_pair() invalid target complex type";

    TokenInt zero(0);
    if ( expr && expr->datadef() && expr->datadef()->is_complex() )
    {
	DataDef *expr_type = expr->datadef();
	DataDefCOMPLEX *expr_cdd = dynamic_cast<DataDefCOMPLEX *>(expr_type);
	if ( !expr_cdd || !expr_cdd->element_type )
	    throw "prepare_complex_component_pair() invalid complex expression type";
	Operand &base = compile_complex_compare_base(pgm, expr, expr_type, base_storage);
	real_storage = complex_component_mem(pgm, base, expr_cdd, false, "__complex_part");
	imag_storage = complex_component_mem(pgm, base, expr_cdd, true, "__complex_part");
	real_part = &real_storage;
	imag_part = &imag_storage;
	real_type = imag_type = expr_cdd->element_type;
	return;
    }

    real_part = &compile_token_normalized(pgm, expr, target_cdd->element_type, nullptr, real_storage);
    imag_part = &compile_token_normalized(pgm, &zero, target_cdd->element_type, nullptr, imag_storage);
    real_type = imag_type = target_cdd->element_type;
}








Operand &compile_call_arg_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
					    bool variadic_real_promotion,
					    Operand &storage, DataDef *&out_type)
{
    bool early_want_cstr = (target_type && target_type->type() == DataType::dtCHARptr
			    && token_is_charptr_expr(token))
		       || (variadic_real_promotion
			   && token_is_charptr_expr(token));
    if ( early_want_cstr )
    {
	Operand &cstr = compile_token_normalized(pgm, token, &ddCHARptr, nullptr, storage);
	storage = cstr;
	out_type = &ddCHARptr;
	return storage;
    }

    regdefp_t argrdp = {nullptr, nullptr, nullptr};
    if ( target_type
      && !(target_type->is_string() && token->datadef() && token->datadef()->is_pointer())
      && !(target_type->is_complex() && token->datadef() && !token->datadef()->is_complex()) )
	argrdp.second = target_type;
    Operand &arg = token->compile(pgm, argrdp);
    DataDef *actual_type = argrdp.second ? argrdp.second : (token->datadef() ? token->datadef() : target_type);
    if ( !actual_type )
	throw "compile_call_arg_normalized() missing argument type";

    bool target_accepts_cstr = type_is_cstr_pointer(target_type)
	|| (target_type
	 && target_type->is_pointer()
	 && target_type->rawtype() == DataType::dtVOID);
    bool want_cstr = (target_accepts_cstr
		      && token->datadef() && token->datadef()->rawtype() == DataType::dtSTRING)
		  || (variadic_real_promotion
		      && token->datadef() && token->datadef()->rawtype() == DataType::dtSTRING);
    if ( want_cstr )
    {
	Operand &cstr = compile_token_normalized(pgm, token, &ddCHARptr, nullptr, storage);
	storage = cstr;
	out_type = target_type && target_type->rawtype() == DataType::dtVOID
	    ? target_type : &ddCHARptr;
	return storage;
    }

    DataDef *final_type = actual_type;
    if ( variadic_real_promotion && actual_type->is_real() )
	final_type = &ddDOUBLE;
    else if ( target_type && target_type->is_complex() )
	final_type = target_type;
    else if ( target_type && (target_type->is_numeric() || target_type->is_pointer()) )
	final_type = target_type;

    if ( final_type && final_type->is_complex()
      && actual_type && !actual_type->is_complex() )
    {
	DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(final_type);
	if ( !cdd || !cdd->element_type )
	    throw "compile_call_arg_normalized() invalid complex target type";
	x86::Mem byval_slot = pgm.cc.newStack((uint32_t)final_type->size, 8);
	IRBuilder ir(pgm.cc);
	x86::Mem real_mem = byval_slot;
	real_mem.setSize((uint32_t)cdd->element_type->size);
	ir.store(IRValue::mem(real_mem, cdd->element_type), ir_from_operand(arg, actual_type));
	x86::Mem imag_mem = byval_slot;
	imag_mem.addOffset((int64_t)cdd->component_offset(true));
	imag_mem.setSize((uint32_t)cdd->element_type->size);
	ir.store(IRValue::mem(imag_mem, cdd->element_type), IRValue::imm(Imm(0), &ddINT));
	storage = as_gp_ptr(pgm, byval_slot, "__call_arg_complex");
	out_type = final_type;
	return storage;
    }

    if ( final_type->is_simd() )
    {
	if ( is_large_simd_type(final_type) )
	{
	    DataDefSIMD *vdd = static_cast<DataDefSIMD *>(final_type);
	    x86::Mem simd_mem = large_simd_expr_mem(pgm, token, vdd, "__call_arg_simd");
	    x86::Mem byval_slot = pgm.cc.newStack((uint32_t)final_type->size, (uint32_t)final_type->alignment());
	    Operand src = simd_mem;
	    emit_raw_aggregate_copy(pgm, byval_slot, src, final_type, "__call_arg_simd_copy");
	    storage = as_gp_ptr(pgm, byval_slot, "__call_arg_simd");
	    out_type = final_type;
	    return storage;
	}
	Operand &simd = compile_token_normalized(pgm, token, final_type, nullptr, storage);
	storage = simd;
	out_type = final_type;
	return storage;
    }

    if ( (final_type->is_numeric() || final_type->is_pointer())
      && (arg.isReg() || arg.isMem() || arg.isImm()) )
    {
	IRBuilder ir(pgm.cc);
	IRValue out = ir.coerce(ir_from_operand(arg, actual_type), final_type);
	out = ir.load(out);
	storage = out.op;
	// Ensure the register width matches the target type. If the
	// expression compiled into a narrower register (e.g. Gpd from
	// a sub-expression that inferred 32-bit type), widen it now so
	// the call-site argument is the correct width.
	if ( storage.isReg() && storage.as<BaseReg>().isGroup(RegGroup::kGp)
	  && final_type->is_integer() && final_type->size == 8
	  && storage.as<x86::Gp>().isGpd() )
	{
	    x86::Gp wide = pgm.cc.newGpq("__call_arg_widen");
	    if ( final_type->is_unsigned() )
		pgm.cc.mov(wide.r32(), storage.as<x86::Gp>());
	    else
		pgm.cc.movsxd(wide, storage.as<x86::Gp>());
	    storage = wide;
	}
	// Call arguments should not keep a Mem operand with a caller-saved
	// base register live across unrelated calls/statements. Materialize
	// numeric/pointer Mem args into fresh registers now so later uses
	// don't depend on a cached base surviving through the surrounding
	// function body. This closes the SMAUG native `bug()` path where
	// `sysdata.log_level` was compiled once, spilled, and its saved
	// base slot was overwritten before the second log_string_plus call.
	if ( storage.isMem() )
	{
	    if ( final_type->is_real() )
	    {
		x86::Xmm tmp = newScalarXmm(pgm, final_type, "__call_arg_f");
		pgm.safemov(tmp, storage.as<x86::Mem>(), final_type, final_type);
		storage = tmp;
	    }
	    else
	    {
		x86::Gp tmp = final_type->is_pointer()
		    ? pgm.cc.newIntPtr("__call_arg_p")
		    : pgm.cc.newGpq("__call_arg_i");
		pgm.safemov(tmp, storage.as<x86::Mem>(), final_type, final_type);
		storage = tmp;
	    }
	}
	out_type = final_type;
	return storage;
    }

    if ( arg.isMem() && actual_type
      && actual_type->basetype() == BaseType::btStruct )
    {
	if ( !aggregate_has_runtime_size(actual_type) && actual_type->size > 0 )
	{
	    x86::Mem byval_slot = pgm.cc.newStack((uint32_t)actual_type->size, 8);
	    emit_raw_aggregate_copy(pgm, byval_slot, arg, actual_type, "__call_arg_copy");
	    storage = as_gp_ptr(pgm, byval_slot, "__call_arg_cls");
	}
	else
	    storage = as_gp_ptr(pgm, arg, "__call_arg_cls");
	out_type = actual_type;
	return storage;
    }
    if ( arg.isReg() && actual_type
      && actual_type->basetype() == BaseType::btStruct )
    {
	if ( !aggregate_has_runtime_size(actual_type) && actual_type->size > 0 )
	{
	    x86::Mem byval_slot = pgm.cc.newStack((uint32_t)actual_type->size, 8);
	    emit_raw_aggregate_copy(pgm, byval_slot, arg, actual_type, "__call_arg_copy");
	    storage = as_gp_ptr(pgm, byval_slot, "__call_arg_cls");
	}
	else
	    storage = arg;
	out_type = actual_type;
	return storage;
    }
    storage = arg;
    out_type = actual_type;
    return storage;
}


Operand &emit_complex_conjugate_expr(Program &pgm, TokenBase *arg_token,
					    DataDef *complex_type,
					    regdefp_t &regdp, Operand &storage)
{
    if ( !arg_token || !complex_type || !complex_type->is_complex() )
	throw "emit_complex_conjugate_expr() expects complex expression";

    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_type);
    if ( !cdd || !cdd->element_type )
	throw "emit_complex_conjugate_expr() invalid complex type";

    Operand arg_base_storage;
    Operand arg_real_storage;
    Operand arg_imag_storage;
    Operand *arg_real = NULL;
    Operand *arg_imag = NULL;
    DataDef *arg_real_type = NULL;
    DataDef *arg_imag_type = NULL;
    prepare_complex_component_pair(pgm, arg_token, complex_type, arg_base_storage,
				   arg_real, arg_real_type, arg_imag, arg_imag_type,
				   arg_real_storage, arg_imag_storage);

    bool use_caller_dest = regdp.first && (!regdp.second || regdp.second->is_complex());
    Operand dest_base;
    if ( use_caller_dest )
	dest_base = *regdp.first;
    else
	dest_base = pgm.cc.newStack((uint32_t)complex_type->size, 8);

    x86::Mem dst_real = complex_component_mem(pgm, dest_base, cdd, false, "__complex_conj_dst");
    x86::Mem dst_imag = complex_component_mem(pgm, dest_base, cdd, true, "__complex_conj_dst");

    IRBuilder ir(pgm.cc);
    IRValue real_val = ir.load(ir.coerce(ir_from_operand(*arg_real, arg_real_type),
					 cdd->element_type));
    ir.store(IRValue::mem(dst_real, cdd->element_type), real_val);

    IRValue imag_val = ir.load(ir.coerce(ir_from_operand(*arg_imag, arg_imag_type),
					 cdd->element_type));
    Operand neg_imag = imag_val.op;
    pgm.safeneg(neg_imag, cdd->element_type);
    ir.store(IRValue::mem(dst_imag, cdd->element_type),
	     IRValue::reg(neg_imag, cdd->element_type));

    if ( use_caller_dest )
    {
	regdp.second = complex_type;
	return *regdp.first;
    }

    storage = dest_base;
    regdp.first = &storage;
    regdp.second = complex_type;
    return storage;
}

// emit_builtin_complex_conjugate and emit_builtin_complex_component
// moved to compiler_builtins.cpp.

void add_funcsig_arg(FuncSignature &funcsig, DataDef *arg_type)
{
    // asmjit caps FuncSignature at kMaxFuncArgs (32). Extra varargs
    // are still passed correctly via the params vector — they just
    // don't need type declarations in the signature.
    if ( funcsig.argCount() >= 32 )
	return;
    if ( !arg_type )
    {
	funcsig.addArgT<void *>();
	return;
    }
    if ( arg_type->is_simd() )
    {
	TypeId tid = simd_type_id(static_cast<DataDefSIMD *>(arg_type));
	if ( tid != TypeId::kVoid )
	{
	    funcsig.addArg(tid);
	    return;
	}
    }
    switch(arg_type->type())
    {
	case DataType::dtCHAR:    funcsig.addArgT<char>(); break;
	case DataType::dtBOOL:    funcsig.addArgT<bool>(); break;
	case DataType::dtINT16:   funcsig.addArgT<int16_t>(); break;
	case DataType::dtINT24:   funcsig.addArgT<int16_t>(); break;
	case DataType::dtINT32:   funcsig.addArgT<int32_t>(); break;
	case DataType::dtINT64:   funcsig.addArgT<int64_t>(); break;
	case DataType::dtUINT8:   funcsig.addArgT<uint8_t>(); break;
	case DataType::dtUINT16:  funcsig.addArgT<uint16_t>(); break;
	case DataType::dtUINT24:  funcsig.addArgT<uint16_t>(); break;
	case DataType::dtUINT32:  funcsig.addArgT<uint32_t>(); break;
	case DataType::dtUINT64:  funcsig.addArgT<uint64_t>(); break;
	case DataType::dtFLOAT:   funcsig.addArgT<float>(); break;
	case DataType::dtDOUBLE:  funcsig.addArgT<double>(); break;
	case DataType::dtCHARptr: funcsig.addArgT<const char *>(); break;
	default:                  funcsig.addArgT<void *>(); break;
    }
}

static void set_funcsig_ret(FuncSignature &funcsig, DataDef *ret_type, bool is_multi_return)
{
    if ( is_multi_return || !ret_type )
    {
	funcsig.setRetT<void>();
	return;
    }
    if ( ret_type->is_simd() )
    {
	TypeId tid = simd_type_id(static_cast<DataDefSIMD *>(ret_type));
	if ( tid != TypeId::kVoid )
	{
	    funcsig.setRet(tid);
	    return;
	}
    }
    switch(ret_type->type())
    {
	case DataType::dtVOID:		funcsig.setRetT<void>();		break;
	case DataType::dtCHAR:		funcsig.setRetT<char>();		break;
	case DataType::dtBOOL:		funcsig.setRetT<bool>();		break;
	case DataType::dtINT16:		funcsig.setRetT<int16_t>();		break;
	case DataType::dtINT24:		funcsig.setRetT<int16_t>();		break;
	case DataType::dtINT32:		funcsig.setRetT<int32_t>();		break;
	case DataType::dtINT64:		funcsig.setRetT<int64_t>();		break;
	case DataType::dtUINT8:		funcsig.setRetT<uint8_t>();		break;
	case DataType::dtUINT16:	funcsig.setRetT<uint16_t>();		break;
	case DataType::dtUINT24:	funcsig.setRetT<uint16_t>();		break;
	case DataType::dtUINT32:	funcsig.setRetT<uint32_t>();		break;
	case DataType::dtUINT64:	funcsig.setRetT<uint64_t>();		break;
	case DataType::dtFLOAT:		funcsig.setRetT<float>();		break;
	case DataType::dtDOUBLE:	funcsig.setRetT<double>();		break;
	case DataType::dtCHARptr:	funcsig.setRetT<const char *>();	break;
	case DataType::dtSTRING:	funcsig.setRetT<void *>();		break;
	default:			funcsig.setRetT<void *>();		break;
    }
}

void append_call_param(std::vector<Operand> &params, FuncSignature &funcsig,
			      const Operand &arg, DataDef *arg_type)
{
    params.push_back(arg);
    add_funcsig_arg(funcsig, arg_type);
}

void set_invoke_arg(Program &pgm, InvokeNode *call, uint32_t &index,
			   const Operand &arg, bool mem_as_address)
{
    // asmjit InvokeNode only has slots for kMaxFuncArgs (32).
    // Extra varargs beyond the signature are still on the stack
    // per the calling convention — just skip setArg for them.
    if ( index >= 32 )
    {
	++index;
	return;
    }
    if ( arg.isReg() && arg.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	call->setArg(index++, arg.as<x86::Xmm>());
	return;
    }
    if ( arg.isReg() )
    {
	call->setArg(index++, arg.as<x86::Gp>());
	return;
    }
    if ( arg.isImm() )
    {
	call->setArg(index++, arg.as<Imm>());
	return;
    }
    if ( arg.isMem() )
    {
	x86::Gp tmp = pgm.cc.newIntPtr("__call_arg");
	if ( mem_as_address )
	    pgm.cc.lea(tmp, arg.as<x86::Mem>());
	else
	    pgm.cc.mov(tmp, arg.as<x86::Mem>());
	call->setArg(index++, tmp);
	return;
    }
    throw "set_invoke_arg() unsupported argument operand";
}

void set_invoke_args(Program &pgm, InvokeNode *call, const std::vector<Operand> &params,
			    bool mem_as_address)
{
    uint32_t argi = 0;
    for ( const Operand &arg : params )
	set_invoke_arg(pgm, call, argi, arg, mem_as_address);
}

static bool call_has_object_arg(TokenCallFunc *call, FuncDef *func, const regdefp_t &regdp)
{
    if ( !regdp.object || !func )
	return false;
    bool has_method_object = dynamic_cast<TokenCallMethod *>(call);
    bool has_stream_object = !has_method_object
			  && func->parameters.size() == call->argc() + 1
			  && !func->parameters.empty()
			  && (func->parameters[0]->has_ostream()
			   || func->parameters[0]->has_istream());
    return has_method_object || has_stream_object;
}

static size_t explicit_expected_argc(FuncDef *func)
{
    size_t expected_argc = func->parameters.size();
    if ( func->is_multi_return() ) expected_argc--;
    if ( func->has_large_struct_retbuf ) expected_argc--;
    if ( func->is_varargs ) expected_argc--;
    return expected_argc;
}

static size_t visible_expected_argc(FuncDef *func, bool has_object_arg)
{
    size_t hidden_argc = 0;
    if ( func->is_multi_return() )
	++hidden_argc;
    if ( func->has_large_struct_retbuf )
	++hidden_argc;
    if ( has_object_arg )
	++hidden_argc;
    if ( func->has_captures )
	++hidden_argc;

    size_t expected_argc = func->parameters.size();
    if ( expected_argc <= hidden_argc )
	expected_argc = 0;
    else
	expected_argc -= hidden_argc;
    if ( func->is_varargs && expected_argc > 0 )
	--expected_argc;
    return expected_argc;
}

static void throw_too_many_call_args(Program &pgm, TokenCallFunc *call, TokenBase *const *parameters,
				     size_t argc)
{
    std::cerr << "ERROR: TokenCallFunc::compile() method " << call->var.name
	      << " called with too many parameters" << std::endl;
    std::cerr << "argc(): " << argc << " func->parameters.size(): "
	      << ((FuncDef *)((Method *)call->var.data)->returns.type)->parameters.size() << std::endl;
    for ( size_t i = 0; i < argc; ++i )
    {
	TokenBase *tn = parameters[i];
	std::cerr << "arg[" << i << "] type() = " << (int)tn->type()
		  << " id() = " << (int)tn->id() << std::endl;
    }
    pgm.Throw(call) << "TokenCallFunc::compile() called with too many parameters" << flush;
}

static void validate_call_arg_type(Program &pgm, TokenBase *tn, DataDef *ptype,
				   DataDef *arg_type, const Operand &tnreg)
{
    if ( !arg_type )
	pgm.Throw(tn) << "Failed to detemine type of rval" << flush;
    if ( ptype->is_numeric() && !arg_type->is_numeric() )
    {
	DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)arg_type->type() << endl);
	pgm.Throw(tn) << "Expecting numeric argument" << flush;
    }
    if ( ptype->is_integer() && !arg_type->is_integer() )
    {
	DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)arg_type->type() << endl);
	pgm.Throw(tn) << "Expecting integer argument" << flush;
    }
    if ( ptype->is_real() && !arg_type->is_real() )
    {
	DBG(cerr << "ptype: " << (int)ptype->type() << " var.type: " << (int)arg_type->type() << endl);
	pgm.Throw(tn) << "Expecting floating point argument" << flush;
    }
    if ( ptype->is_simd() && !arg_type->is_simd() )
	pgm.Throw(tn) << "Expecting SIMD argument" << flush;
    if ( ptype->is_string() && !arg_type->is_string() && !arg_type->is_pointer() )
	pgm.Throw(tn) << "Expecting string argument" << flush;
    if ( ptype->is_object() && !arg_type->is_pointer() )
    {
	if ( !arg_type->is_object() )
	    pgm.Throw(tn) << "Expecting object argument" << flush;
	if ( ptype->rawtype() != arg_type->rawtype() )
	    pgm.Throw(tn) << "Object type mismatch" << flush;
    }
    if ( tnreg.isReg() && tnreg.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	if ( !ptype->is_real() && !ptype->is_simd() )
	    pgm.Throw(tn) << "Not expecting floating point argument" << flush;
	DBG(pgm.cc.comment("tnreg is Xmm"));
    }
    if ( tnreg.isReg() && tnreg.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( ptype->is_real() || (ptype->is_simd() && !is_large_simd_type(ptype)) )
	    pgm.Throw(tn) << "Expecting floating point argument" << flush;
	DBG(pgm.cc.comment("tnreg is Gp"));
	DBG(cout << "tnreg size=" << tnreg.x86RmSize() << " regdp.second->size="
		 << arg_type->size << " type " << arg_type->name << endl);
    }
    if ( tnreg.isImm() )
	pgm.cc.comment("tnreg is Imm");
}

void *runtime_eval_scope_context_set_int(void *ctx, void *key, int64_t value)
{
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(value));
    return ctx;
}

void *runtime_eval_scope_context_set_real(void *ctx, void *key, double value)
{
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(value));
    return ctx;
}

void *runtime_eval_scope_context_set_string(void *ctx, void *key, void *value)
{
    const char *s = (const char *)value;
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(std::string(s ? s : "")));
    return ctx;
}

void *runtime_eval_scope_context_set_array(void *ctx, void *key, void *value)
{
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(*(MadArray *)value));
    return ctx;
}

static void emit_runtime_eval_scope_setter(Program &pgm,
					   x86::Gp ctx_ptr,
					   Variable *scope_var,
					   Variable *key_var)
{
    if ( !scope_var || !scope_var->type || !key_var )
	return;

    TokenVar key_token(*key_var);
    Operand key_storage;
    DataDef *key_type = NULL;
    Operand &key_arg = compile_call_arg_normalized(pgm, &key_token, &ddSTRING, false, key_storage, key_type);

    DataType raw = scope_var->type->rawtype();
    TokenVar value_token(*scope_var);

    if ( raw == DataType::dtBOOL || scope_var->type->is_integer() )
    {
	Operand value_storage;
	DataDef *value_type = NULL;
	Operand &value_arg = compile_call_arg_normalized(pgm, &value_token, &ddINT64, false, value_storage, value_type);
	InvokeNode *call;
	pgm.cc.invoke(&call, imm(runtime_eval_scope_context_set_int), FuncSignature::build<void *, void *, void *, int64_t>());
	call->setArg(0, ctx_ptr);
	call->setArg(1, key_arg.as<x86::Gp>());
	if ( value_arg.isReg() )
	    call->setArg(2, value_arg.as<x86::Gp>());
	else if ( value_arg.isImm() )
	    call->setArg(2, value_arg.as<Imm>());
	else
	{
	    x86::Gp tmp = pgm.cc.newGpq("__scope_i64");
	    pgm.cc.mov(tmp, value_arg.as<x86::Mem>());
	    call->setArg(2, tmp);
	}
	return;
    }

    if ( scope_var->type->is_real() )
    {
	Operand value_storage;
	DataDef *value_type = NULL;
	Operand &value_arg = compile_call_arg_normalized(pgm, &value_token, &ddDOUBLE, false, value_storage, value_type);
	InvokeNode *call;
	pgm.cc.invoke(&call, imm(runtime_eval_scope_context_set_real), FuncSignature::build<void *, void *, void *, double>());
	call->setArg(0, ctx_ptr);
	call->setArg(1, key_arg.as<x86::Gp>());
	if ( value_arg.isReg() )
	    call->setArg(2, value_arg.as<x86::Xmm>());
	else
	{
	    x86::Xmm tmp = newScalarXmm(pgm, &ddDOUBLE, "__scope_f64");
	    pgm.cc.movsd(tmp, value_arg.as<x86::Mem>());
	    call->setArg(2, tmp);
	}
	return;
    }

    if ( raw == DataType::dtSTRING )
    {
	Operand value_storage;
	DataDef *value_type = NULL;
	Operand &value_arg = compile_call_arg_normalized(pgm, &value_token, &ddCHARptr, false, value_storage, value_type);
	InvokeNode *call;
	pgm.cc.invoke(&call, imm(runtime_eval_scope_context_set_string), FuncSignature::build<void *, void *, void *, void *>());
	call->setArg(0, ctx_ptr);
	call->setArg(1, key_arg.as<x86::Gp>());
	call->setArg(2, value_arg.as<x86::Gp>());
	return;
    }

    if ( raw == DataType::dtARRAY )
    {
	Operand value_storage;
	DataDef *value_type = NULL;
	Operand &value_arg = compile_call_arg_normalized(pgm, &value_token, &ddARRAY, false, value_storage, value_type);
	InvokeNode *call;
	pgm.cc.invoke(&call, imm(runtime_eval_scope_context_set_array), FuncSignature::build<void *, void *, void *, void *>());
	call->setArg(0, ctx_ptr);
	call->setArg(1, key_arg.as<x86::Gp>());
	call->setArg(2, value_arg.as<x86::Gp>());
    }
}



// construct a string at ptr address
void *string_construct(void *ptr)
{
    DBG(cout << "string_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::string;
}

extern "C" void *__madc_aot_init_string(void *ptr, const char *src)
{
    DBG(cout << "__madc_aot_init_string(" << (uint64_t)ptr
	     << ", " << (uint64_t)src << ')' << endl);
    return new(ptr) std::string(src ? src : "");
}

extern "C" void *__madc_aot_init_cout(void *ptr)
{
    DBG(cout << "__madc_aot_init_cout(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::ostream(std::cout.rdbuf());
}

extern "C" void *__madc_aot_init_cerr(void *ptr)
{
    DBG(cout << "__madc_aot_init_cerr(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::ostream(std::cerr.rdbuf());
}

extern "C" void *__madc_aot_init_cin(void *ptr)
{
    DBG(cout << "__madc_aot_init_cin(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::istream(std::cin.rdbuf());
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

// construct a MadArray at ptr address
void *madarray_construct(void *ptr)
{
    DBG(cout << "madarray_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) MadArray;
}

// destruct a MadArray at ptr address
void madarray_destruct(void *ptr)
{
    DBG(cout << "madarray_destruct(" << (uint64_t)ptr << ')' << endl);
    ((MadArray *)ptr)->~MadArray();
}

// construct/destruct file streams
void *ifstream_construct(void *ptr)
{
    DBG(cout << "ifstream_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::ifstream;
}
void ifstream_destruct(void *ptr)
{
    DBG(cout << "ifstream_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::ifstream *)ptr)->~ifstream();
}
void *ofstream_construct(void *ptr)
{
    DBG(cout << "ofstream_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::ofstream;
}
void ofstream_destruct(void *ptr)
{
    DBG(cout << "ofstream_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::ofstream *)ptr)->~ofstream();
}
void *fstream_construct(void *ptr)
{
    DBG(cout << "fstream_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::fstream;
}
void fstream_destruct(void *ptr)
{
    DBG(cout << "fstream_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::fstream *)ptr)->~fstream();
}

// file stream methods
void fstream_open(void *ptr, void *filename)
{
    ((std::fstream *)ptr)->open(((std::string *)filename)->c_str());
}
void ifstream_open(void *ptr, void *filename)
{
    ((std::ifstream *)ptr)->open(((std::string *)filename)->c_str());
}
void ofstream_open(void *ptr, void *filename)
{
    ((std::ofstream *)ptr)->open(((std::string *)filename)->c_str());
}
void fstream_close(void *ptr)
{
    ((std::fstream *)ptr)->close();
}
void ifstream_close(void *ptr)
{
    ((std::ifstream *)ptr)->close();
}
void ofstream_close(void *ptr)
{
    ((std::ofstream *)ptr)->close();
}
// separate typed versions needed because ios is a virtual base —
// casting void* to ios* directly gives the wrong pointer offset
int64_t ifstream_eof(void *ptr)  { return ((std::ifstream *)ptr)->eof() ? 1 : 0; }
int64_t ifstream_good(void *ptr) { return ((std::ifstream *)ptr)->good() ? 1 : 0; }
int64_t ifstream_is_open(void *ptr) { return ((std::ifstream *)ptr)->is_open() ? 1 : 0; }
int64_t ofstream_good(void *ptr) { return ((std::ofstream *)ptr)->good() ? 1 : 0; }
int64_t ofstream_is_open(void *ptr) { return ((std::ofstream *)ptr)->is_open() ? 1 : 0; }
int64_t fstream_eof(void *ptr)   { return ((std::fstream *)ptr)->eof() ? 1 : 0; }
int64_t fstream_good(void *ptr)  { return ((std::fstream *)ptr)->good() ? 1 : 0; }
int64_t fstream_is_open(void *ptr) { return ((std::fstream *)ptr)->is_open() ? 1 : 0; }



// return c_str() pointer from a std::string — used when passing a string to a const char* param
const char *string_cstr(void *ptr)
{
    return ((std::string *)ptr)->c_str();
}

// call string assign method, TODO: call directly
void string_assign(std::string &o, std::string &n)
{
    DBG(cout << "string_assign(" << o << '['<< (uint64_t)&o << "], " << n << '[' << (uint64_t)&n << "])" << endl);
    o.assign(n);
    DBG(cout << "string_assign(" << o << '['<< (uint64_t)&o << "])" << endl);
    DBG(cout << "string_assign(" << o << "::c_str()["<< (uint64_t)o.c_str() << "])" << endl);
}



void streamout_int(std::ostream &os, int i)
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



void istream_construct(void *ptr, void *init)
{
    new(ptr) std::istream((std::streambuf *)init);
}

void istream_destruct(void *ptr)
{
    ((std::istream *)ptr)->~istream();
}

// extern declarations for php array helpers (defined in ns_php.cpp)
extern int64_t php_count(void *arr);
extern void *php_array_get(void *result, void *arr, int64_t index);
extern int64_t php_array_get_int(void *arr, int64_t index);

// extern declarations for STL container helpers (defined in ns_stl.cpp)
extern void *vector_int_construct(void *);
extern void  vector_int_destruct(void *);
extern void *vector_str_construct(void *);
extern void  vector_str_destruct(void *);
extern int64_t vector_int_size(void *);
extern int64_t vector_int_at(void *, int64_t);
extern void vector_int_set(void *, int64_t, int64_t);
extern void *vector_str_at(void *, void *, int64_t);
extern void vector_str_set(void *, int64_t, void *);
extern int64_t vector_str_size(void *);
extern void map_str_int_set(void *, void *, int64_t);
extern int64_t map_str_int_get(void *, void *);
extern void map_str_str_set(void *, void *, void *);
extern void *map_str_str_get(void *, void *, void *);
extern void *map_str_int_construct(void *);
extern void  map_str_int_destruct(void *);
extern void *map_str_str_construct(void *);
extern void  map_str_str_destruct(void *);
extern void *set_str_construct(void *);
extern void  set_str_destruct(void *);
extern void *set_int_construct(void *);
extern void  set_int_destruct(void *);
extern void *list_int_construct(void *);
extern void  list_int_destruct(void *);
extern void *list_str_construct(void *);
extern void  list_str_destruct(void *);

MadcAsmjitErrHandler::MadcAsmjitErrHandler() : pgm(nullptr), hits(0) {}

// Diagnostic ErrorHandler for MADC_VALIDATE: prints the asmjit error
// at the moment of emit (kValidateIntermediate active) so the offending
// instruction is localized to the current source token.
void MadcAsmjitErrHandler::handleError(asmjit::Error err, const char *message,
				       asmjit::BaseEmitter *)
{
    if ( ++hits > 30 ) return; // cap noise on cascades
    std::cerr << "[asmjit] err=" << err << " ("
	      << asmjit::DebugUtils::errorAsString(err) << "): "
	      << (message ? message : "")
	      << std::endl;
    if ( pgm )
    {
	if ( !pgm->cur_func_name.empty() )
	    std::cerr << "  in function: " << pgm->cur_func_name << std::endl;
	TokenBase *tb = pgm->curToken();
	if ( tb )
	    std::cerr << "  at curToken file="
		      << (tb->file ? tb->file : "(null)")
		      << " " << tb->line << ":" << tb->column << std::endl;
    }
}

void Program::record_compile_anchor(TokenBase *tb, const char *kind)
{
    if ( !jit_source_map_enabled || !tb || !tb->file ) return;
    asmjit::Label l = cc.newLabel();
    cc.bind(l);
    JitSourceEntry e;
    e.byte_offset = 0;
    e.file = tb->file;
    e.line = (uint32_t)tb->line;
    e.col = (uint16_t)tb->column;
    e.kind = kind;
    jit_anchor_labels.push_back(std::make_pair(l, e));
}

void Program::emit_data_mov(x86::Gp &dst, void *data_ptr)
{
    if ( aot_tracking )
    {
	Label lbl = cc.newLabel();
	cc.bind(lbl);
	AotDataRef ref;
	ref.label_id = lbl.id();
	ref.address = reinterpret_cast<uintptr_t>(data_ptr);
	ref.data_offset = (size_t)-1;
	ref.imm_offset = 2;
	aot_data_refs.push_back(ref);
    }
    cc.mov(dst, imm(data_ptr));
}

void Program::emit_data_mov(x86::Gp &dst, Variable *var, size_t extra_offset)
{
    void *data_ptr = (var && var->data)
	? reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(var->data) + extra_offset)
	: NULL;
    if ( aot_tracking && var )
	record_aot_variable_data(var);
    if ( var )
    {
	const char *debug_var = ::getenv("MADC_DEBUG_AOT_VAR");
	if ( debug_var && var->name == debug_var )
	{
	    size_t layout_offset = 0;
	    bool layout_found = data_ptr
		? lookup_aot_data_offset(reinterpret_cast<uintptr_t>(data_ptr), layout_offset)
		: false;
	    std::fprintf(stderr,
		"[aot] emit_data_mov var=%s data=%p has_aot=%d aot_off=%zu extra=%zu layout_found=%d layout_off=%zu\n",
		var->name.c_str(), data_ptr, var->has_aot_data() ? 1 : 0,
		var->aot_data_offset, extra_offset, layout_found ? 1 : 0, layout_offset);
	}
    }
    if ( aot_tracking && var )
    {
	Label lbl = cc.newLabel();
	cc.bind(lbl);
	AotDataRef ref;
	ref.label_id = lbl.id();
	ref.address = reinterpret_cast<uintptr_t>(data_ptr);
	ref.data_offset = (size_t)-1;
	if ( var->has_aot_data() )
	    ref.data_offset = var->aot_data_offset + extra_offset;
	else
	{
	    size_t layout_offset = 0;
	    if ( data_ptr && lookup_aot_data_offset(reinterpret_cast<uintptr_t>(data_ptr), layout_offset) )
		ref.data_offset = layout_offset;
	    else if ( tkProgram )
	    {
		Variable *canon = tkProgram->findVariable(var->name);
		if ( canon && canon->has_aot_data() )
		    ref.data_offset = canon->aot_data_offset + extra_offset;
	    }
	}
	ref.imm_offset = 2;
	aot_data_refs.push_back(ref);
    }
    cc.mov(dst, imm(data_ptr));
}

void Program::track_invoke_target(void *addr)
{
    if ( !aot_tracking || !addr )
	return;
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if ( external_symbol_map.count(a) )
	return;
    // Try to find the name via dladdr.
    Dl_info info;
    if ( dladdr(addr, &info) && info.dli_sname && info.dli_sname[0] )
	external_symbol_map[a] = info.dli_sname;
}

void Program::_compiler_init()
{
    code.reset();
    code.init(jit.environment());

    jit_anchor_labels.clear();
    jit_source_map.clear();
    if ( const char *env = ::getenv("MADC_NO_SOURCE_MAP") )
	if ( env[0] && env[0] != '0' )
	    jit_source_map_enabled = false;
    DBG(
        static FileLogger logger(stdout);
        logger.setFlags(FormatFlags::kMachineCode);
        code.setLogger(&logger);
    );
//  this seems to break things at times
//  code.addEmitterOptions(BaseEmitter::kOptionStrictValidation);
    code.attach(&cc);
    if ( const char *env = ::getenv("MADC_VALIDATE") )
    {
	if ( env[0] && env[0] != '0' )
	{
	    cc.addDiagnosticOptions(asmjit::DiagnosticOptions::kValidateIntermediate);
	    asmjit_err_handler.pgm = this;
	    asmjit_err_handler.hits = 0;
	    code.setErrorHandler(&asmjit_err_handler);
	}
    }
    if ( const char *envlog = ::getenv("MADC_DUMP_ASM") )
    {
	if ( envlog[0] )
	{
	    static FILE *fp = fopen(envlog, "w");
	    if ( fp )
	    {
		static asmjit::FileLogger logger(fp);
		logger.addFlags(asmjit::FormatFlags::kMachineCode
			      | asmjit::FormatFlags::kExplainImms
			      | asmjit::FormatFlags::kRegCasts);
		code.setLogger(&logger);
	    }
	}
    }
    // constant initialization
//  __const_double_1 = cc.newDoubleConst(ConstPool::kScopeGlobal, 1.0);
}

bool Program::_compiler_finalize()
{
    asmjit::Error ferr = cc.finalize();
    if (ferr)
	std::cerr << "cc.finalize() error=" << ferr << " ("
		  << asmjit::DebugUtils::errorAsString(ferr) << ")" << std::endl;
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
    // Backfill x86code only for the known user-defined functions/lambdas that
    // participated in the funcnode pre-pass. Scanning every global btFunct
    // symbol is unsafe for large C headers because prototypes / typedef-like
    // declarations can be "function-shaped" without carrying a real Method.
    for ( TokenBase *tb_func : pending_funcs )
    {
	TokenFunc *tf = dynamic_cast<TokenFunc *>(tb_func);
	if ( !tf || !tf->var.data || tf->is_overridden )
	    continue;

	Method *method = (Method *)tf->var.data;
	if ( !method || method->x86code )
	    continue;

	FuncDef *func = dynamic_cast<FuncDef *>(method->returns.type);
	if ( !func || !func->funcnode )
	    continue;

	method->x86code = (uint8_t *)root_fn + code.labelOffset(func->funcnode->label());
    }

    // Build the JIT-PC → source-line map. Each anchor was a label
    // bound at a Token::compile() entry; resolve each to a byte offset
    // in the code section and sort by offset. CLI/worker layers can
    // inspect Program::jit_source_map directly when they want to map
    // a faulting RIP back to source.
    if ( jit_source_map_enabled && !jit_anchor_labels.empty() )
    {
	jit_source_map.clear();
	jit_source_map.reserve(jit_anchor_labels.size());
	for ( auto &pr : jit_anchor_labels )
	{
	    if ( !code.isLabelBound(pr.first) ) continue;
	    JitSourceEntry e = pr.second;
	    e.byte_offset = (uint32_t)code.labelOffset(pr.first);
	    jit_source_map.push_back(e);
	}
	std::sort(jit_source_map.begin(), jit_source_map.end(),
		  [](const JitSourceEntry &a, const JitSourceEntry &b) {
		      return a.byte_offset < b.byte_offset;
		  });
	// Optional dump for debugging the source map itself
	if ( const char *envmap = ::getenv("MADC_DUMP_SOURCEMAP") )
	{
	    if ( envmap[0] )
	    {
		FILE *fp = fopen(envmap, "w");
		if ( fp )
		{
		    fprintf(fp, "# %zu anchors, code base=%p size=%zu\n",
			    jit_source_map.size(), (const void *)root_fn, code.codeSize());
		    for ( auto &e : jit_source_map )
			fprintf(fp, "+0x%-8x %s:%u:%u (%s)\n",
				e.byte_offset,
				e.file ? e.file : "(null)",
				(unsigned)e.line, (unsigned)e.col,
				e.kind ? e.kind : "?");
		    fclose(fp);
		}
	    }
	}
    }
    jit_anchor_labels.clear();

    if ( const char *envfinal = ::getenv("MADC_DUMP_FINAL") )
    {
	if ( envfinal[0] )
	{
	    FILE *fp = fopen(envfinal, "w");
	    if ( fp )
	    {
		// Collect (start_addr, name) tuples for every user-defined
		// function whose JIT'd code we know the address of.
		std::vector<std::pair<uint8_t *, std::string>> functions;
		size_t total = 0, unbound = 0;
		for ( TokenBase *tb_func : pending_funcs )
		{
		    TokenFunc *tf = dynamic_cast<TokenFunc *>(tb_func);
		    if ( !tf || !tf->var.data ) continue;
		    Method *method = (Method *)tf->var.data;
		    if ( !method || !method->x86code ) continue;
		    total++;
		    FuncDef *fd = dynamic_cast<FuncDef *>(method->returns.type);
		    bool bound = (fd && fd->funcnode &&
				  code.isLabelBound(fd->funcnode->label()));
		    if ( !bound ) { unbound++; continue; }
		    functions.push_back(std::make_pair(
			(uint8_t *)method->x86code, tf->var.name));
		}
		fprintf(fp, "# total user funcs in pending_funcs=%zu, "
			    "unbound-label=%zu (skipped from per-func dump)\n",
			total, unbound);
		std::sort(functions.begin(), functions.end());
		uint8_t *code_end = (uint8_t *)root_fn + code.codeSize();
		fprintf(fp, "# MADC_DUMP_FINAL: %zu user functions, "
			    "code base=%p size=%zu\n",
			functions.size(), root_fn, code.codeSize());
		// Index header: name -> start, length
		fprintf(fp, "# index:\n");
		for ( size_t i = 0; i < functions.size(); i++ )
		{
		    uint8_t *start = functions[i].first;
		    uint8_t *end = (i + 1 < functions.size())
				   ? functions[i + 1].first : code_end;
		    fprintf(fp, "#   %p +%-6zu  %s\n",
			    (void *)start, (size_t)(end - start),
			    functions[i].second.c_str());
		}
		fprintf(fp, "\n");
		// Per-function byte dump
		for ( size_t i = 0; i < functions.size(); i++ )
		{
		    uint8_t *start = functions[i].first;
		    uint8_t *end = (i + 1 < functions.size())
				   ? functions[i + 1].first : code_end;
		    size_t len = (end > start) ? (size_t)(end - start) : 0;
		    fprintf(fp, "=== %s @ %p (size=%zu) ===\n",
			    functions[i].second.c_str(),
			    (void *)start, len);
		    for ( size_t j = 0; j < len; j++ )
		    {
			if ( (j & 15) == 0 ) fprintf(fp, "%04zx: ", j);
			fprintf(fp, "%02x ", start[j]);
			if ( (j & 15) == 15 ) fprintf(fp, "\n");
		    }
		    if ( (len & 15) != 0 ) fprintf(fp, "\n");
		    fprintf(fp, "\n");
		}
		fclose(fp);
	    }
	}
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

    // Builtin dispatch — handles __builtin_*, _chk family, overflow helpers,
    // abs family, complex number functions, and simple libc redirects.
    {
	Operand *builtin_result = NULL;
	if ( try_compile_builtin(pgm, this, regdp, _operand, builtin_result) )
	    return *builtin_result;
    }

    // function pointer call — indirect invoke through address in variable
    if ( var.type->is_function() && var.type->is_numeric() )
    {
	DBG(cout << "TokenCallFunc::compile() function pointer call through " << var.name << endl);
	DBG(pgm.cc.comment("function pointer call"));

	DataDefFPTR *fptr = static_cast<DataDefFPTR *>(var.type);
	FuncDef *func = fptr ? fptr->target : NULL;
	if ( !func && src_node )
	{
	    DataDefFPTR *src_fptr = dynamic_cast<DataDefFPTR *>(src_node->datadef());
	    if ( src_fptr )
		func = src_fptr->target;
	}
	if ( !func )
	    pgm.Throw(this) << "function pointer call missing target signature" << flush;
	FuncSignature funcsig(CallConvId::kCDecl);
	bool ret_is_variadic = false;
	bool fptr_large_struct_ret = !func->is_multi_return()
	    && is_large_struct_return(&func->returns);

	set_funcsig_ret(funcsig, &func->returns,
	    func->is_multi_return() || fptr_large_struct_ret);
	DataDef &retdd = func->returns;

	// Load the function pointer. When src_node is set (direct struct-member
	// invocation like cmd.fn(args)), compile it to materialise the fn-ptr
	// value in a register — var is a placeholder Variable the member created
	// at parse time and has no real storage to voperand from.
	//
	// Hold the result in an Operand local rather than juggling a Gp*-to-
	// Operand* reinterpret cast; both branches write a pointer-shaped
	// value into `ptr_op` which the invoke below treats as a Gp.
	Operand ptr_op;
	if ( src_node )
	{
	    DBG(pgm.cc.comment("fptr call source: src_node (struct-member fptr)"));
	    regdefp_t src_rdp = {NULL, NULL, NULL};
	    Operand &src_op = src_node->compile(pgm, src_rdp);
	    x86::Gp ptr_gp = pgm.cc.newIntPtr("__fptr_member");
	    if ( src_op.isMem() )
		pgm.cc.mov(ptr_gp, src_op.as<x86::Mem>());
	    else
		pgm.cc.mov(ptr_gp, src_op.as<x86::Gp>());
	    ptr_op = ptr_gp;
	}
	else
	{
	    Operand &var_op = pgm.tkFunction->voperand(pgm, &var);
	    // Stack-backed fn-pointer variables land here as Mem — load into
	    // a Gp so the invoke below can dispatch through it. Register-
	    // backed vars pass through untouched.
	    if ( var_op.isMem() )
	    {
		x86::Gp ptr_gp = pgm.cc.newIntPtr("__fptr_var");
		pgm.cc.mov(ptr_gp, var_op.as<x86::Mem>());
		ptr_op = ptr_gp;
	    }
	    else
		ptr_op = var_op;
	}

	// compile arguments and build signature
	std::vector<Operand> params;

	// Large struct return through function pointer: allocate retbuf
	// and pass as hidden first arg, same as the direct call path.
	x86::Gp fptr_retbuf_ptr;
	if ( fptr_large_struct_ret )
	{
	    DBG(pgm.cc.comment("fptr large struct return: allocate retbuf"));
	    x86::Mem retbuf = pgm.cc.newStack((uint32_t)func->returns.size, 8);
	    fptr_retbuf_ptr = pgm.cc.newIntPtr("__fptr_retbuf");
	    pgm.cc.lea(fptr_retbuf_ptr, retbuf);
	    append_call_param(params, funcsig, fptr_retbuf_ptr, &ddINT64);
	}

	// [&] lambda capture: env_ptr declared here so post-call reload can access it
	x86::Gp env_ptr = pgm.cc.newIntPtr("__env_ptr");
	pgm.cc.xor_(env_ptr, env_ptr);

	if ( func->has_captures )
	{
	    add_funcsig_arg(funcsig, NULL); // env pointer (first arg)
	    size_t n = func->captures.size();
	    if ( n > 0 )
	    {
		x86::Mem env_stack = pgm.cc.newStack((uint32_t)(n * 8), 8);
		pgm.cc.lea(env_ptr, env_stack);
		for ( size_t ci = 0; ci < n; ++ci )
		{
		    std::string cap_name = func->captures[ci].name;
		    Variable *cap_var = pgm.tkFunction->findVariable(cap_name);
		    if ( !cap_var ) continue;
		    Operand &cap_op = pgm.tkFunction->voperand(pgm, cap_var);
		    DataDef *cap_type = func->captures[ci].type;
		    // Numeric captures: store the VALUE in env[ci].
		    // Non-numeric (string/object) captures: store the ADDRESS
		    // of the outer variable in env[ci] — the callee dereferences
		    // on each access.
		    x86::Gp slot_val = pgm.cc.newGpq(cap_type->is_numeric() ? "__cap_val" : "__cap_str");
		    if ( cap_type->is_numeric() )
			load_var_to_gp(pgm, cap_op, slot_val);
		    else
			lea_var_to_gp(pgm, cap_op, slot_val);
		    pgm.cc.mov(x86::qword_ptr(env_ptr, (int64_t)ci * 8), slot_val);
		}
	    }
	    params.push_back(env_ptr);
	}

	size_t expected_argc = explicit_expected_argc(func);
	// K&R-style `()` (unspecified params) allows any argument count.
	bool kr_unspecified = func->parameters.empty() && !func->is_void_params;
	if ( !func->is_varargs && !kr_unspecified && argc() > expected_argc )
	    throw_too_many_call_args(pgm, this, parameters.data(), argc());
	size_t fixed_argc = (func->is_varargs || kr_unspecified) ? expected_argc : argc();
	for ( size_t i = 0; i < fixed_argc; ++i )
	{
	    // skip env param (index 0 in func->parameters) when capturing
	    size_t fi = func->has_captures ? i + 1 : i;
	    DataDef *ptype = fi < func->parameters.size() ? func->parameters[fi] : &ddINT64;
	    DataDef *arg_type = NULL;
	    Operand arg_storage;
	    Operand &areg = compile_call_arg_normalized(pgm, parameters[i], ptype, false, arg_storage, arg_type);
	    append_call_param(params, funcsig, areg, arg_type);
	}
	if ( func->is_varargs )
	{
	    if ( argc() < fixed_argc )
		pgm.Throw(this) << "function pointer varargs call missing fixed arguments" << flush;
	    if ( funcsig.argCount() >= 1 )
		funcsig.setVaIndex((uint32_t)funcsig.argCount());
	    for ( size_t i = fixed_argc; i < argc(); ++i )
	    {
		DataDef *arg_type = NULL;
		Operand arg_storage;
		Operand &areg = compile_call_arg_normalized(pgm, parameters[i], NULL, true, arg_storage, arg_type);
		append_call_param(params, funcsig, areg, arg_type);
	    }
	}

	// invoke through register
	InvokeNode *call;
	pgm.cc.invoke(&call, ptr_op.as<x86::Gp>(), funcsig);
	set_invoke_args(pgm, call, params, false);

	// reload numeric captures from env back to outer variables (copy-out semantics)
	if ( func->has_captures )
	{
	    for ( size_t ci = 0; ci < func->captures.size(); ++ci )
	    {
		DataDef *cap_type = func->captures[ci].type;
		if ( !cap_type->is_numeric() ) continue;
		std::string cap_name = func->captures[ci].name;
		Variable *cap_var = pgm.tkFunction->findVariable(cap_name);
		if ( !cap_var ) continue;
		Operand &cap_op = pgm.tkFunction->voperand(pgm, cap_var);
		x86::Gp val = pgm.cc.newGpq("__cap_reload");
		pgm.cc.mov(val, x86::qword_ptr(env_ptr, (int64_t)ci * 8));
		store_gp_to_var(pgm, val, cap_op);
	    }
	}

	// Large struct return: buffer already filled by callee
	if ( fptr_large_struct_ret )
	{
	    DBG(pgm.cc.comment("fptr large struct ret: return retbuf address"));
	    _operand = fptr_retbuf_ptr;
	    if ( !regdp.first ) regdp.first = &_operand;
	    regdp.second = &func->returns;
	    return _operand;
	}
	// capture return value
	if ( retdd.type() != DataType::dtVOID )
	{
	    if ( !regdp.first )
	    {
		if ( retdd.is_real() || retdd.is_simd() )
		    _operand = retdd.newreg(pgm.cc, "fptr_ret");
		else
		    _operand = pgm.cc.newGpq("fptr_ret");
		regdp.first = &_operand;
	    }
	    bind_call_return(pgm, call, regdp.first, &retdd, _operand, ret_is_variadic);
	}
	regdp.second = &func->returns;

	return *regdp.first;
    }

    Method *method;
    FuncNode *fnd;

    // grab the Method object
    if ( !(method=(Method *)var.data) )
	pgm.Throw(this) << "TokenCallFunc::compile() function method is NULL" << flush;

    // grab the FuncNode object
    if ( !(fnd=((FuncDef *)(method->returns.type))->funcnode) && !method->x86code )
    {
	// dlcall: special case — call through function pointer (first arg)
	if ( var.name == "dlcall" )
	{
	    if ( argc() < 1 )
		pgm.Throw(this) << "dlcall requires at least a function pointer argument" << flush;

	    FuncDef *func = (FuncDef *)method->returns.type;
	    FuncSignature funcsig(CallConvId::kCDecl);
	    set_funcsig_ret(funcsig, &func->returns, func->is_multi_return());

	    // compile the first arg — the function pointer
	    regdefp_t ptrrdp = {NULL, NULL, NULL};
	    Operand &ptr_reg = parameters[0]->compile(pgm, ptrrdp);

	    // compile remaining args
	    std::vector<Operand> params;
	    for ( size_t i = 1; i < argc(); ++i )
	    {
		TokenBase *tn = parameters[i];
		DataDef *arg_type = NULL;
		Operand arg_storage;
		Operand &areg = compile_call_arg_normalized(pgm, tn, NULL, true, arg_storage, arg_type);
		append_call_param(params, funcsig, areg, arg_type);
	    }

	    // invoke through the function pointer register
	    InvokeNode *call;
	    pgm.cc.invoke(&call, ptr_reg.as<x86::Gp>(), funcsig);
	    set_invoke_args(pgm, call, params, false);

	    // capture return value (Mem dests are honored via bind_call_return)
	    if ( !regdp.first )
	    {
		_operand = pgm.cc.newGpq("dlcall_ret");
		regdp.first = &_operand;
	    }
	    bind_call_return(pgm, call, regdp.first, &func->returns, _operand, /*is_variadic=*/false);
	    if ( !regdp.second )
		regdp.second = &func->returns;

	    return *regdp.first;
	}
	// Last-chance dlsym: an `extern RET name(args);` forward
	// declaration registers `name` as a function but doesn't run
	// dlsym (only undeclared identifiers hit the parse-time
	// fallback). Try resolving here so user code can typed-call
	// any libc / system symbol just by extern-declaring it.
	if ( !pgm.is_dynamic_symbol_fallback_enabled() )
	    pgm.Throw(this) << "dynamic symbol fallback is disabled by registration policy" << flush;
	std::string symbol_name = typed_overflow_predicate_symbol_name(var, parameters);
	if ( !pgm.is_dynamic_symbol_allowed(symbol_name) )
	    pgm.Throw(this) << "dynamic symbol '" << var.name
			    << "' is not allowed by registration policy" << flush;
	void *sym = dlsym(RTLD_DEFAULT, symbol_name.c_str());
		if ( sym )
		{
		    method->x86code = sym;
		    pgm.external_symbol_map[reinterpret_cast<uintptr_t>(sym)] =
			external_symbol_export_name(symbol_name, sym);
		    fnd = NULL; // intentional — fall into the typed-call path below
		    DBG(pgm.cc.comment("TokenCallFunc::compile() dlsym late-bind"));
		}
	else
	    pgm.Throw(this) << "TokenCallFunc::compile(" << var.name
			    << ") method has neither FuncNode nor x86code" << flush;
    }

    // External C varargs (`extern int sprintf(char*, const char*, ...);`)
    // must use the platform varargs ABI, not madc's internal hidden
    // __va_args packing convention. Build a real variadic signature
    // from the declared fixed params plus the actual trailing args.
    if ( !fnd && method->x86code && ((FuncDef *)method->returns.type)->is_varargs )
    {
	FuncDef *func = (FuncDef *)method->returns.type;
	FuncSignature funcsig(CallConvId::kCDecl);
	set_funcsig_ret(funcsig, &func->returns, false);

	std::vector<Operand> params;
	size_t fixed_argc = explicit_expected_argc(func);
	for ( size_t i = 0; i < argc(); ++i )
	{
	    DataDef *ptype = i < fixed_argc && i < func->parameters.size()
		? func->parameters[i] : NULL;
	    TokenBase *tn = parameters[i];
	    DataDef *arg_type = NULL;
	    Operand arg_storage;
	    Operand &areg = compile_call_arg_normalized(pgm, tn, ptype,
		i >= fixed_argc, arg_storage, arg_type);
	    append_call_param(params, funcsig, areg, arg_type);
	}
	funcsig.setVaIndex((uint32_t)fixed_argc);

	InvokeNode *call;
	pgm.cc.invoke(&call, imm(method->x86code), funcsig);
	pgm.track_invoke_target(method->x86code);
	set_invoke_args(pgm, call, params, false);

	bool is_narrow_int_ret = is_int32_dlsym_ret(&func->returns, var.name);
	if ( !regdp.first )
	{
	    if ( func->returns.is_real() )
		_operand = newScalarXmm(pgm, &func->returns, "dl_ret");
	    else
		_operand = pgm.cc.newGpq("dl_ret");
	    regdp.first = &_operand;
	}
	bind_call_return(pgm, call, regdp.first, &func->returns, _operand,
			 /*is_variadic=*/func->returns.is_real(),
			 is_narrow_int_ret);
	if ( !regdp.second )
	    regdp.second = &func->returns;
	return *regdp.first;
    }

    // variadic dlsym call: no funcnode, has x86code, 0 declared params
    // build signature from actual arg types (like dlcall)
    if ( !fnd && method->x86code )
    {
	FuncDef *func = (FuncDef *)method->returns.type;
	if ( func->parameters.empty() )
	{
	    FuncSignature funcsig(CallConvId::kCDecl);

	    // compile args to determine types
	    bool has_double_args = false;
	    std::vector<Operand> params;
	    std::vector<DataDef *> param_types;
	    for ( size_t i = 0; i < argc(); ++i )
	    {
		TokenBase *tn = parameters[i];
		DataDef *arg_type = NULL;
		Operand arg_storage;
		Operand &areg = compile_call_arg_normalized(pgm, tn, NULL, true, arg_storage, arg_type);
		params.push_back(areg);
		param_types.push_back(arg_type);
		if ( arg_type && arg_type->type() == DataType::dtDOUBLE )
		    has_double_args = true;
	    }

	    // Set return type BEFORE adding arg types. The default
	    // heuristic ("any double arg → double return") is wrong for
	    // the printf family — they return `int` even with double args.
	    // Telling asmjit the return is `double` makes its register
	    // allocator keep an xmm reg live across the call, which can
	    // interfere with arg xmm setup. Use the int-returner whitelist
	    // to suppress the override for known int-returning libc funcs.
	    bool actually_returns_int = is_int32_dlsym_ret(&func->returns, var.name);
	    bool ret_double = !actually_returns_int
		&& (has_double_args
		    || (regdp.first && regdp.first->isReg()
			&& regdp.first->as<BaseReg>().isGroup(RegGroup::kVec)));
	    if ( ret_double )
		funcsig.setRetT<double>();
	    else
		funcsig.setRetT<int64_t>();

	    // add arg types to signature
	    for ( DataDef *param_type : param_types )
		add_funcsig_arg(funcsig, param_type);

	    // The variadic dlsym path resolves printf-family functions
	    // (printf / fprintf / sprintf / snprintf / ...). Per SysV
	    // x86-64 ABI, calls to variadic functions must set AL = number
	    // of XMM registers used. Without telling asmjit the signature
	    // is variadic, AL is left with whatever was in rax (often the
	    // format-string pointer's low byte) — printf reads it and may
	    // skip xmm0, printing 0.0 for `%f` arguments instead of the
	    // real value. Setting vaIndex marks the args at and after
	    // that index as variadic; index 1 (everything past the first
	    // fixed arg, e.g. the format string) is correct for the
	    // entire printf family.
	    if ( funcsig.argCount() >= 1 )
		funcsig.setVaIndex(1);

	    // invoke
	    InvokeNode *call;
	    pgm.cc.invoke(&call, imm(method->x86code), funcsig);
	    pgm.track_invoke_target(method->x86code);
	    set_invoke_args(pgm, call, params, false);

	    // capture return value
	    if ( ret_double )
	    {
		if ( !regdp.first )
		{
		    _operand = newScalarXmm(pgm, &ddDOUBLE, "dl_ret");
		    regdp.first = &_operand;
		}
		bind_call_return(pgm, call, regdp.first, &ddDOUBLE, _operand, /*is_variadic=*/true);
		regdp.second = &ddDOUBLE;
	    }
	    else
	    {
		bool is_narrow_int_ret = is_int32_dlsym_ret(&func->returns, var.name);
		if ( !regdp.first )
		{
		    _operand = pgm.cc.newGpq("dl_ret");
		    regdp.first = &_operand;
		}
		bind_call_return(pgm, call, regdp.first, &func->returns, _operand,
				 /*is_variadic=*/false, is_narrow_int_ret);
		if ( !regdp.second )
		    regdp.second = &func->returns;
	    }

	    return *regdp.first;
	}
    }

    // build arguments
    FuncDef *func = (FuncDef *)method->returns.type;
    bool has_object_arg = call_has_object_arg(this, func, regdp);
    FuncSignature funcsig(CallConvId::kCDecl);
    std::vector<Operand> params;
    DataDef *ptype;
    TokenBase *tn;
    bool is_variadic = func->parameters.empty() && method->x86code;

    // Always set regdp.second to the function's actual return type.
    // The caller may have set it to a target type (e.g. ddDOUBLE for
    // a float-returning function), but callers like compile_token_normalized
    // need the real return type to know what the register actually holds.
    DBG(cout << "TokenCallFunc::compile(" << var.name << ") regdp.second = " << func->returns.name << endl);
    regdp.second = &func->returns;

    DBG(cout << "TokenCallFunc::compile(" << var.name << ") func->returns.type() " << (int)func->returns.type() << endl);

    bool large_struct_ret = !func->is_multi_return()
	&& is_large_struct_return(&func->returns);
    set_funcsig_ret(funcsig, &func->returns,
	func->is_multi_return() || large_struct_ret);
    x86::Gp large_retbuf_ptr;
    if ( func->is_multi_return() )
    {
	if ( !regdp.object )
	    throw "TokenCallFunc::compile() multi-return missing __retbuf object";
	if ( regdp.object->isMem() )
	{
	    x86::Gp retbuf_ptr = pgm.cc.newIntPtr("__retbuf_arg");
	    pgm.cc.lea(retbuf_ptr, regdp.object->as<x86::Mem>());
	    append_call_param(params, funcsig, retbuf_ptr, &ddINT64);
	}
	else
	    append_call_param(params, funcsig, *regdp.object, &ddINT64);
	DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(__retbuf)"));
    }
    else if ( large_struct_ret )
    {
	// Allocate stack buffer for large struct return, pass as hidden first arg
	DBG(pgm.cc.comment("large struct return: allocate retbuf"));
	x86::Mem retbuf = pgm.cc.newStack((uint32_t)func->returns.size, 8);
	large_retbuf_ptr = pgm.cc.newIntPtr("__large_retbuf");
	pgm.cc.lea(large_retbuf_ptr, retbuf);
	append_call_param(params, funcsig, large_retbuf_ptr, &ddINT64);
    }
//#if OBJECT_SUPPORT
    // pass along object ("this") as first argument if appropriate
    if ( has_object_arg )
    {
	// for struct/class objects on the stack (Mem), pass the address via LEA
	if ( regdp.object->isMem() )
	{
	    x86::Gp obj_ptr = pgm.cc.newIntPtr("__obj_ptr");
	    pgm.cc.lea(obj_ptr, regdp.object->as<x86::Mem>());
	    append_call_param(params, funcsig, obj_ptr, NULL);
	}
	else
	    append_call_param(params, funcsig, *regdp.object, NULL);
	DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(*regdp.object)"));
    }
//#endif

    x86::Gp env_ptr = pgm.cc.newIntPtr("__env_ptr");
    pgm.cc.xor_(env_ptr, env_ptr);
    if ( func->has_captures )
    {
	add_funcsig_arg(funcsig, NULL);
	size_t n = func->captures.size();
	if ( n > 0 )
	{
	    x86::Mem env_stack = pgm.cc.newStack((uint32_t)(n * 8), 8);
	    pgm.cc.lea(env_ptr, env_stack);
	    for ( size_t ci = 0; ci < n; ++ci )
	    {
		std::string cap_name = func->captures[ci].name;
		Variable *cap_var = pgm.tkFunction->findVariable(cap_name);
		if ( !cap_var ) continue;
		Operand &cap_op = pgm.tkFunction->voperand(pgm, cap_var);
		DataDef *cap_type = func->captures[ci].type;
		x86::Gp slot_val = pgm.cc.newGpq(cap_type->is_numeric() ? "__cap_val" : "__cap_str");
		if ( cap_type->is_numeric() )
		    load_var_to_gp(pgm, cap_op, slot_val);
		else
		    lea_var_to_gp(pgm, cap_op, slot_val);
		pgm.cc.mov(x86::qword_ptr(env_ptr, (int64_t)ci * 8), slot_val);
	    }
	}
	append_call_param(params, funcsig, env_ptr, &ddINT64);
    }

    size_t expected_argc = visible_expected_argc(func, has_object_arg);
    // K&R functions with empty param list (not `void`) accept any number of args
    bool knr_unspecified = func->parameters.empty() && !func->is_void_params;
    if ( !is_variadic && !func->is_varargs && !knr_unspecified && argc() > expected_argc )
	throw_too_many_call_args(pgm, this, parameters.data(), argc());

    size_t param_offset = (func->is_multi_return() ? 1 : 0)
	+ (func->has_large_struct_retbuf ? 1 : 0)
	+ (has_object_arg ? 1 : 0)
	+ (func->has_captures ? 1 : 0);
    size_t fixed_argc = (func->is_varargs || knr_unspecified) ? expected_argc : argc();
    for ( size_t i = 0; i < fixed_argc; ++i )
    {
	size_t pi = i + param_offset;
	ptype = pi < func->parameters.size() ? func->parameters[pi] : &ddINT64;
	tn = parameters[i];
	DataDef *arg_type = NULL;
	Operand arg_storage;

	DBG(pgm.cc.comment("TokenCallFunc::argc param"));
	// Reference parameter: pass address instead of value
	bool is_ref_param = pi < func->ref_params.size() && func->ref_params[pi];
	if ( is_ref_param )
	{
	    // Get the storage location of the argument, then LEA
	    x86::Gp addr = pgm.cc.newIntPtr("__ref_arg");
	    TokenVar *tv = dynamic_cast<TokenVar *>(tn);
	    if ( tv )
	    {
		// Variable argument: get its operand (Mem or Gp) and LEA
		Operand &var_op = tv->operand(pgm);
		if ( var_op.isMem() )
		    pgm.cc.lea(addr, var_op.as<x86::Mem>());
		else
		    pgm.cc.mov(addr, var_op.as<x86::Gp>());
	    }
	    else
	    {
		// Expression argument: compile, spill to stack, then LEA
		regdefp_t ref_rdp = {NULL, NULL, NULL};
		Operand &ref_val = tn->compile(pgm, ref_rdp);
		if ( ref_val.isMem() )
		    pgm.cc.lea(addr, ref_val.as<x86::Mem>());
		else
		{
		    x86::Mem spill = pgm.cc.newStack(8, 8);
		    if ( ref_val.isReg() )
			pgm.cc.mov(spill, ref_val.as<x86::Gp>());
		    pgm.cc.lea(addr, spill);
		}
	    }
	    arg_type = ptype;
	    append_call_param(params, funcsig, addr, arg_type);
	}
	else
	{
	Operand &tnreg = compile_call_arg_normalized(pgm, tn, ptype, is_variadic, arg_storage, arg_type);
	validate_call_arg_type(pgm, tn, ptype, arg_type, tnreg);
	DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(tnreg)"));
	// could probably use a tv->var.addArgT(funcsig) method
	DBG(pgm.cc.comment(ptype->name.c_str() /*arg_type->name.c_str()*/));
	append_call_param(params, funcsig, tnreg, arg_type);
	}
	if ( arg_type->type() == DataType::dtDOUBLE )
	    DBG(pgm.cc.comment("addArgT<double>()"));
    }

    // varargs call site: pack extra arguments into a stack buffer, pass as __va_args
    if ( func->is_varargs && argc() > fixed_argc )
    {
	size_t nvarargs = argc() - fixed_argc;
	size_t va_buf_size = 0;
	for ( size_t i = 0; i < nvarargs; ++i )
	{
	    DataDef *va_type = parameters[fixed_argc + i]
		? parameters[fixed_argc + i]->datadef() : NULL;
	    va_buf_size += internal_vararg_static_slot_size(va_type);
	}
	x86::Mem va_buf = pgm.cc.newStack((uint32_t)va_buf_size, 8);
	x86::Gp va_ptr = pgm.cc.newIntPtr("__va_buf");
	pgm.cc.lea(va_ptr, va_buf);
	size_t va_off = 0;

	for ( size_t i = 0; i < nvarargs; ++i )
	{
	    TokenBase *tn_va = parameters[fixed_argc + i];
	    DataDef *va_type = NULL;
	    Operand va_storage;
	    Operand &va_reg = compile_call_arg_normalized(pgm, tn_va, NULL, true, va_storage, va_type);
	    x86::Mem slot = va_buf;
	    slot.addOffset((int64_t)va_off);
	    size_t slot_size = internal_vararg_static_slot_size(va_type);
	    if ( va_type && aggregate_has_runtime_size(va_type) )
	    {
		x86::Gp src_ptr = materialize_gp_ptr_arg(pgm, va_reg, "__va_pack_dyn");
		slot.setSize(8);
		pgm.cc.mov(slot, src_ptr);
	    }
	    else
	    if ( va_type && !va_type->is_numeric() && !va_type->is_pointer() && !va_type->is_real() )
	    {
		slot.setSize((uint32_t)slot_size);
		emit_raw_aggregate_copy(pgm, slot, va_reg, va_type, "__va_pack_copy");
	    }
	    else
	    {
		x86::Gp val = pgm.cc.newGpq("va_pack");
		if ( va_reg.isReg() && va_reg.as<BaseReg>().isGroup(RegGroup::kGp) )
		{
		    x86::Gp gp = va_reg.as<x86::Gp>();
		    if ( gp.size() < 8 )
		    {
			if ( va_type && va_type->is_unsigned() )
			    pgm.cc.movzx(val, gp);
			else
			    pgm.cc.movsx(val, gp);
		    }
		    else
			pgm.cc.mov(val, gp);
		}
		else if ( va_reg.isReg() && va_reg.as<BaseReg>().isGroup(RegGroup::kVec) )
		{
		    // varargs reals are normalized to double; store raw 8 bytes
		    pgm.cc.movq(val, va_reg.as<x86::Xmm>());
		}
		else if ( va_reg.isMem() )
		    pgm.cc.mov(val, va_reg.as<x86::Mem>());
		else if ( va_reg.isImm() )
		    pgm.cc.mov(val, va_reg.as<Imm>());

		slot.setSize(8);
		pgm.cc.mov(slot, val);
	    }
	    va_off += slot_size;
	}

	append_call_param(params, funcsig, va_ptr, &ddINT64); // __va_args: pointer to packed buffer
    }
    else if ( func->is_varargs )
    {
	// no extra args — pass NULL for __va_args
	x86::Gp null_ptr = pgm.cc.newIntPtr("__va_null");
	pgm.cc.mov(null_ptr, 0);
	append_call_param(params, funcsig, null_ptr, &ddINT64);
    }

    if ( !fnd )
	DBG(std::cout << "TokenCallFunc::compile(cc.call(" << (uint64_t)method->x86code << ')' << std::endl);

    // when pointer args were passed for string params, redirect to C library
    // version via dlsym (the built-in wrapper expects std::string*)
    void *call_target = method->x86code;
    if ( !fnd && is_overflow_predicate_helper_name(var.name) )
    {
	std::string symbol_name = typed_overflow_predicate_symbol_name(var, parameters);
	void *sym = dlsym(RTLD_DEFAULT, symbol_name.c_str());
	if ( sym )
	{
	    call_target = sym;
	    pgm.external_symbol_map[reinterpret_cast<uintptr_t>(sym)] =
		external_symbol_export_name(symbol_name, sym);
	}
    }
    bool use_c_version = false;
    for ( size_t i = 0; i < argc(); ++i )
    {
	size_t pi = i + param_offset;
	DataDef *pt = pi < func->parameters.size() ? func->parameters[pi] : NULL;
	if ( pt && pt->is_string() && parameters[i]->datadef() && parameters[i]->datadef()->is_pointer() )
	{
	    use_c_version = true;
	    break;
	}
    }
    if ( use_c_version && !fnd )
    {
	std::string symbol_name = typed_overflow_predicate_symbol_name(var, parameters);
	if ( !pgm.is_dynamic_symbol_allowed(symbol_name) )
	    pgm.Throw(this) << "dynamic symbol '" << var.name
			    << "' is not allowed by registration policy" << flush;
	void *sym = dlsym(RTLD_DEFAULT, symbol_name.c_str());
		if ( sym )
		{
		    call_target = sym;
		    pgm.external_symbol_map[reinterpret_cast<uintptr_t>(sym)] =
			external_symbol_export_name(symbol_name, sym);
		    DBG(cout << "TokenCallFunc::compile() redirecting " << var.name << " to C library version" << endl);
		}
    }

    // now we should have all we need to call the function
    DBG(pgm.cc.comment("pgm.call:"));
    DBG(pgm.cc.comment(var.name.c_str()));
    InvokeNode *call;
    DBG(cout << "invoke: argCount=" << funcsig.argCount() << " hasRet=" << funcsig.hasRet() << " is_variadic=" << is_variadic << endl);
    // AOT mode: builtin functions with x86code addresses must be called
    // via imm(x86code) — not via fnd->label() — so the address enters
    // the addrtab/relocation system and gets resolved to the real symbol
    // name (mangled) in libmadc.so. Label-based calls are JIT-internal
    // and don't create external symbol relocations.
    if ( fnd && !use_c_version && !(pgm.aot_tracking && method->x86code) )
	pgm.cc.invoke(&call, fnd->label(), funcsig);
    else if ( is_variadic )
    {
	x86::Gp fn_ptr = pgm.cc.newIntPtr("dl_fn");
	pgm.cc.mov(fn_ptr, imm(call_target));
	pgm.cc.invoke(&call, fn_ptr, funcsig);
    }
    else
	pgm.cc.invoke(&call, imm(call_target), funcsig);
    if ( call_target )
	pgm.track_invoke_target(call_target);
    DBG(pgm.cc.comment("TokenCallFunc::compile() looping over params"));
    set_invoke_args(pgm, call, params, false);

    if ( func->has_captures )
    {
	for ( size_t ci = 0; ci < func->captures.size(); ++ci )
	{
	    DataDef *cap_type = func->captures[ci].type;
	    if ( !cap_type->is_numeric() ) continue;
	    std::string cap_name = func->captures[ci].name;
	    Variable *cap_var = pgm.tkFunction->findVariable(cap_name);
	    if ( !cap_var ) continue;
	    Operand &cap_op = pgm.tkFunction->voperand(pgm, cap_var);
	    x86::Gp val = pgm.cc.newGpq("__cap_reload");
	    pgm.cc.mov(val, x86::qword_ptr(env_ptr, (int64_t)ci * 8));
	    store_gp_to_var(pgm, val, cap_op);
	}
    }

    DBG(std::cout << "TokenCallFunc::compile() END" << std::endl);

    if ( !regdp.second )
	regdp.second = &func->returns;

    // Small struct return-by-value (1..16 bytes) per SysV x86-64:
    // the callee places bytes 0..7 in rax, bytes 8..15 in rdx. Madc's
    // FuncSignature can only declare a single TypeId for the return,
    // so cc.invoke alone captures rax. Without explicit handling, the
    // caller treats the rax value as a pointer (e.g. for the struct
    // memcpy in TokenAssign) and segfaults dereferencing arbitrary
    // bytes. Spill rax (and rdx for >8-byte structs) to a fresh stack
    // slot here, then return the slot's address as the operand —
    // that is the address downstream struct copies expect.
    // Large struct return (>16 bytes): buffer was pre-allocated and
    // passed as hidden first arg.  The data is already in the buffer.
    if ( large_struct_ret )
    {
	DBG(pgm.cc.comment("TokenCallFunc::compile() large struct ret via retbuf"));
	_operand = large_retbuf_ptr;
	if ( !regdp.first ) regdp.first = &_operand;
	return _operand;
    }

    if ( func->returns.basetype() == BaseType::btStruct
      && func->returns.size > 0 && func->returns.size <= 16 )
    {
	DBG(pgm.cc.comment("TokenCallFunc::compile() small struct ret spill"));
	x86::Gp rax_v = pgm.cc.newGpq("structret_lo");
	call->setRet(0, rax_v);

	uint32_t slot_size = (uint32_t)((func->returns.size + 7u) & ~7u);
	if ( slot_size < 16 ) slot_size = 16;
	x86::Mem slot = pgm.cc.newStack(slot_size, 8);
	x86::Mem lo = slot; lo.setSize(8);
	pgm.cc.mov(lo, rax_v);

	if ( func->returns.size > 8 )
	{
	    // Capture rdx into a vreg before asmjit's allocator reuses it.
	    // The mov is the first instruction emitted after the InvokeNode,
	    // so rdx still holds the high half of the SysV return.
	    x86::Gp rdx_v = pgm.cc.newGpq("structret_hi");
	    pgm.cc.mov(rdx_v, x86::rdx);
	    x86::Mem hi = slot; hi.setSize(8); hi.addOffset(8);
	    pgm.cc.mov(hi, rdx_v);
	}

	x86::Gp slot_addr = pgm.cc.newIntPtr("structret_addr");
	pgm.cc.lea(slot_addr, slot);
	_operand = slot_addr;
	if ( !regdp.first ) regdp.first = &_operand;
	return _operand;
    }

    // For a dlsym late-bound extern-declared function, apply the same
    // int32-sign-extension whitelist the variadic dlsym path uses, so
    // `extern int strcmp(...)` returns negative values correctly.
    bool typed_narrow_int_ret = (!fnd && method->x86code)
				&& is_int32_dlsym_ret(&func->returns, var.name);
#if 1
    // handle return value
    if ( regdp.first )
    {
	bind_call_return(pgm, call, regdp.first, &func->returns, _operand,
			 is_variadic, typed_narrow_int_ret);
	DBG(pgm.cc.comment("TokenCallFunc::compile() regdp.first END"));
	return *regdp.first;
    }
    else
#endif
    if ( func->returns.type() != DataType::dtVOID )
    {
	bind_call_return(pgm, call, NULL, &func->returns, _operand,
			 is_variadic, typed_narrow_int_ret);
	regdp.first = &_operand;
    }
    DBG(pgm.cc.comment("TokenCallFunc::compile() END"));

    return _operand;
}

Operand &TokenScopeContext::compile(Program &pgm, regdefp_t &regdp)
{
    Operand &ctx = pgm.tkFunction->voperand(pgm, &context_var);
    if ( !ctx.isReg() || !ctx.as<BaseReg>().isGroup(RegGroup::kGp) )
	pgm.Throw(this) << "runtime eval scope context requires an addressable array object" << flush;

    x86::Gp ctx_ptr = ctx.as<x86::Gp>();
    InvokeNode *reset_dtor;
    pgm.cc.invoke(&reset_dtor, imm(madarray_destruct), FuncSignature::build<void, void *>());
    reset_dtor->setArg(0, ctx_ptr);
    InvokeNode *reset_ctor;
    pgm.cc.invoke(&reset_ctor, imm(madarray_construct), FuncSignature::build<void *, void *>());
    reset_ctor->setArg(0, ctx_ptr);

    for ( std::size_t i = 0; i < scope_vars.size(); ++i )
    {
	Variable *scope_var = scope_vars[i];
	if ( !scope_var )
	    continue;
	std::string key = scope_var->name;
	Variable *key_var = pgm.addLiteral(key);
	emit_runtime_eval_scope_setter(pgm, ctx_ptr, scope_var, key_var);
    }

    _operand = ctx_ptr;
    regdp.first = &_operand;
    regdp.second = &ddARRAY;
    return _operand;
}

Operand &TokenCpnd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") TOP" << endl);
    auto invalidate_rematerializable_globals = [&]() {
	for ( std::map<Variable *, Operand>::iterator it = operand_map.begin();
	      it != operand_map.end(); )
	{
	    Variable *var = it->first;
	    if ( var && var->is_global() && (var->data || var->has_aot_data())
	      && (var->is_fixed_array()
	       || var->type->basetype() == BaseType::btStruct
	       || var->type->basetype() == BaseType::btClass) )
	    {
		it = operand_map.erase(it);
		continue;
	    }
	    ++it;
	}
    };
    for ( vector<TokenStmt *>::iterator vti = statements.begin(); vti != statements.end(); ++vti )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	pgm.record_compile_anchor(*vti, "stmt");
	(*vti)->compile(pgm, regdp);
	invalidate_rematerializable_globals();
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

    pgm.cc.addFunc(FuncSignature::build<void>());

    auto invalidate_rematerializable_globals = [&]() {
	for ( std::map<Variable *, Operand>::iterator it = operand_map.begin();
	      it != operand_map.end(); )
	{
	    Variable *var = it->first;
	    if ( var && var->is_global() && (var->data || var->has_aot_data())
	      && (var->is_fixed_array()
	       || var->type->basetype() == BaseType::btStruct
	       || var->type->basetype() == BaseType::btClass) )
	    {
		it = operand_map.erase(it);
		continue;
	    }
	    ++it;
	}
    };

    for ( vector<TokenStmt *>::iterator si = statements.begin(); si != statements.end(); ++si )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	pgm.record_compile_anchor(*si, "init");
	(*si)->compile(pgm, regdp);
	invalidate_rematerializable_globals();
    }

    pgm.tkFunction->cleanup(pgm);	// cleanup stack
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
	case TokenType::ttChar:
	    DBG(cout << "TokenStmt::compile() TokenChar(" << (char)ival() << ')' << endl);
	    return dynamic_cast<TokenChar *>(this)->compile(pgm, regdp);
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
	case TokenType::ttStructLit:
	{
	    // C99 compound literal: (type){ init_list }
	    // Allocate a temporary on the stack, initialize members, return address.
	    TokenStructLit *slit = dynamic_cast<TokenStructLit *>(this);
	    DataDef *dd = slit->datadef();
	    DataDefSTRUCT *sdd = dd ? dynamic_cast<DataDefSTRUCT *>(dd) : NULL;
	    if ( sdd && sdd->size == 0 )
	    {
		for ( TokenBase *init : slit->inits )
		{
		    if ( !init )
			continue;
		    regdefp_t side_rdp = {nullptr, nullptr, nullptr};
		    init->compile(pgm, side_rdp);
		}
		x86::Gp zero = pgm.cc.newIntPtr("_compound_empty");
		pgm.cc.xor_(zero, zero);
		_operand = zero;
		bool want_struct_value =
		    regdp.second
		    && regdp.second->basetype() == BaseType::btStruct
		    && dd->basetype() == BaseType::btStruct;
		regdp.second = want_struct_value ? dd : pgm.getPointerType(dd);
		regdp.first = &_operand;
		return _operand;
	    }
	    if ( sdd && sdd->size > 0 )
	    {
		x86::Gp base = pgm.cc.newIntPtr("_compound_lit");
		if ( pgm.tkFunction == pgm.tkProgram )
		{
		    // File-scope compound literals have static storage duration.
		    // Stack temps here leave globals holding dead addresses once
		    // startup init finishes.
		    void *storage = calloc(1, (size_t)sdd->size);
		    if ( pgm.aot_tracking )
		    {
			std::string name = "__aot_compound_lit_"
			    + std::to_string(pgm.aot_discovered_data.size());
			pgm.record_aot_data(name, storage, (size_t)sdd->size, dd);
		    }
		    pgm.emit_data_mov(base, storage);
		}
		else
		{
		    x86::Mem tmp = pgm.cc.newStack((uint32_t)sdd->size, 8);
		    pgm.cc.lea(base, tmp);
		}
		emit_zero_fill_region(pgm, base, 0, sdd->size);
		emit_struct_init(pgm, base, 0, sdd, slit->inits, this);
		_operand = base;
		DataDef *ptr_type = pgm.getPointerType(dd);
		bool want_struct_value =
		    regdp.second
		    && regdp.second->basetype() == BaseType::btStruct
		    && dd->basetype() == BaseType::btStruct;
		if ( !want_struct_value && regdp.first && regdp.first != &_operand )
		{
		    pgm.safemov(*regdp.first, _operand, ptr_type, ptr_type);
		    regdp.second = ptr_type;
		    return *regdp.first;
		}
		regdp.second = want_struct_value ? dd : ptr_type;
		regdp.first = &_operand;
		return _operand;
	    }
	    if ( dd && dd->is_simd() && dd->size > 0 )
	    {
		DataDefSIMD *vdd = dynamic_cast<DataDefSIMD *>(dd);
		if ( !vdd )
		    throw "compound literal: invalid SIMD type";
		x86::Gp base = pgm.cc.newIntPtr("_compound_simd");
		if ( pgm.tkFunction == pgm.tkProgram )
		{
		    void *storage = calloc(1, dd->size);
		    if ( pgm.aot_tracking )
		    {
			std::string name = "__aot_compound_simd_"
			    + std::to_string(pgm.aot_discovered_data.size());
			pgm.record_aot_data(name, storage, dd->size, dd);
		    }
		    pgm.emit_data_mov(base, storage);
		}
		else
		{
		    x86::Mem tmp = pgm.cc.newStack((uint32_t)dd->size, (uint32_t)vdd->alignment());
		    pgm.cc.lea(base, tmp);
		}
		emit_zero_fill_region(pgm, base, 0, dd->size);
		emit_simd_init(pgm, base, 0, vdd, slit->inits, this);
		if ( is_large_simd_type(dd) )
		{
		    x86::Mem mem = x86::ptr(base, 0, (uint32_t)dd->size);
		    _operand = mem;
		    regdp.second = dd;
		    regdp.first = &_operand;
		    return _operand;
		}
		if ( regdp.second && !regdp.second->is_simd()
		  && regdp.second->is_integer() && regdp.second->size <= dd->size )
		{
		    x86::Gp gp = pgm.cc.newGpq("_compound_simd_int");
		    pgm.cc.mov(gp, x86::qword_ptr(base));
		    _operand = gp;
		    regdp.first = &_operand;
		    return _operand;
		}
		x86::Xmm xmm = pgm.cc.newXmm("_compound_simd_xmm");
		x86::Mem src = x86::ptr(base, 0, (uint32_t)dd->size);
		load_mem_to_xmm(pgm, xmm, src, dd);
		_operand = xmm;
		regdp.second = dd;
		if ( regdp.first && regdp.first != &_operand )
		{
		    pgm.safemov(*regdp.first, _operand, dd, dd);
		    return *regdp.first;
		}
		regdp.first = &_operand;
		return _operand;
	    }
	    throw "compound literal: unsupported type";
	}
	case TokenType::ttProgram:
	    return dynamic_cast<TokenProgram *>(this)->compile(pgm, regdp);
	case TokenType::ttSymbol:
	    if ( id() == TokenID::tkSemi )
	    {
		DBG(cout << "TokenStmt::compile() TokenSymbol(;) NOOP" << endl);
		break;
	    }
	default:
	    DBG(cerr << "TokenStmt::compile() throwing unexpected token type=" << (int)type() << " id=" << (int)id() << endl);
	    {
		static char msg[128];
		snprintf(msg, sizeof(msg), "TokenStmt::compile() unexpected token type=%d id=%d",
			 (int)type(), (int)id());
		throw (const char *)msg;
	    }
    } // end switch
    DBG(cout << "TokenStmt::compile(" << (void *)this << ") END" << endl);
    return _reg;
}

// Emit per-member writes for a struct init block into [base_reg + base_ofs + member_ofs].
// Used for both standalone struct init and each element of an array-of-structs.
void emit_simd_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
    DataDefSIMD *vdd, const std::vector<TokenBase *> &inits, TokenBase *err_loc)
{
    if ( !vdd || !vdd->element_type || vdd->element_type->size == 0 )
	pgm.Throw(err_loc) << "invalid SIMD type in initializer" << flush;
    size_t elem_size = vdd->element_type->size;
    for ( size_t i = 0; i < vdd->lane_count; ++i )
    {
	x86::Mem slot = x86::ptr(base_reg, base_ofs + (int32_t)(i * elem_size), (uint32_t)elem_size);
	if ( i >= inits.size() )
	{
	    pgm.cc.mov(slot, imm(0));
	    continue;
	}
	regdefp_t elem_rdp = {nullptr, vdd->element_type, nullptr};
	Operand &val = inits[i]->compile(pgm, elem_rdp);
	if ( val.isImm() )
	    pgm.cc.mov(slot, val.as<Imm>());
	else if ( val.isReg() && val.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    x86::Xmm xmm = val.as<x86::Xmm>();
	    if ( elem_size == sizeof(float) )
		pgm.cc.movss(slot, xmm);
	    else
		pgm.cc.movsd(slot, xmm);
	}
	else if ( val.isReg() && val.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    x86::Gp gp = val.as<x86::Gp>();
	    if ( elem_size == 8 ) pgm.cc.mov(slot, gp.r64());
	    else if ( elem_size == 4 ) pgm.cc.mov(slot, gp.r32());
	    else if ( elem_size == 2 ) pgm.cc.mov(slot, gp.r16());
	    else pgm.cc.mov(slot, gp.r8());
	}
	else
	    pgm.Throw(err_loc) << "unsupported SIMD literal element" << flush;
    }
}

static void emit_struct_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
    DataDefSTRUCT *dds, const std::vector<TokenBase *> &inits, TokenBase *err_loc)
{
    // Union with anonymous struct init: {{"1234", "567"}} wraps the real
    // initializers in a TokenStructLit that represents the anonymous struct
    // group.  After addAnonymousAggregate() flattened the members, unwrap
    // the struct literal so its children map to the consecutive members.
    const std::vector<TokenBase *> *effective_inits = &inits;
    std::vector<TokenBase *> unwrapped;
    if ( dds->union_layout && inits.size() == 1 )
    {
	TokenStructLit *slit = dynamic_cast<TokenStructLit *>(inits[0]);
	if ( slit && dds->has_anon_aggregate )
	{
	    unwrapped = slit->inits;
	    effective_inits = &unwrapped;
	}
    }
    const std::vector<TokenBase *> &eff = *effective_inits;

    size_t init_idx = 0;
    for ( size_t mi = 0; mi < dds->members.size() && init_idx < eff.size(); ++mi )
    {
	auto &mp = dds->members[mi];
	DataDef *mtype = mp.second;
	int32_t addr = base_ofs + (int32_t)((mi < dds->member_offsets.size())
	    ? dds->member_offsets[mi] : 0);
	size_t i = init_idx;
	size_t member_count = (mi < dds->member_counts.size()) ? dds->member_counts[mi] : 1;

	// Flat init for array members: distribute consecutive initializers
	if ( member_count > 1 && eff[init_idx] == NULL )
	{
	    size_t esize = mtype->size;
	    for ( size_t ai = 0; ai < member_count; ++ai )
	    {
		x86::Mem mm = x86::ptr(base_reg,
		    addr + (int32_t)(ai * esize), (uint32_t)esize);
		pgm.cc.mov(mm, imm(0));
	    }
	    ++init_idx;
	    continue;
	}
	if ( member_count > 1 && dynamic_cast<TokenStructLit *>(eff[init_idx]) == NULL )
	{
	    std::string char_array_init;
	    if ( type_is_cstr_element(mtype)
	      && token_get_constant_cstring(eff[init_idx], char_array_init) )
	    {
		size_t copy_n = char_array_init.size();
		if ( copy_n > member_count )
		    copy_n = member_count;
		for ( size_t ai = 0; ai < copy_n; ++ai )
		{
		    x86::Mem mm = x86::ptr(base_reg, addr + (int32_t)ai, 1);
		    pgm.cc.mov(mm, imm((int32_t)(unsigned char)char_array_init[ai]));
		}
		for ( size_t ai = copy_n; ai < member_count; ++ai )
		{
		    x86::Mem mm = x86::ptr(base_reg, addr + (int32_t)ai, 1);
		    pgm.cc.mov(mm, imm(0));
		}
		++init_idx;
		continue;
	    }
	    size_t esize = mtype->size;
	    for ( size_t ai = 0; ai < member_count && init_idx < eff.size(); ++ai, ++init_idx )
	    {
		int32_t elem_addr = addr + (int32_t)(ai * esize);
		if ( !eff[init_idx] )
		{
		    x86::Mem mm = x86::ptr(base_reg, elem_addr, (uint32_t)esize);
		    pgm.cc.mov(mm, imm(0));
		    continue;
		}
		regdefp_t rdp = {nullptr, nullptr, nullptr};
		rdp.second = mtype;
		Operand &val = eff[init_idx]->compile(pgm, rdp);
		x86::Mem mm = x86::ptr(base_reg, elem_addr, (uint32_t)esize);
		if ( val.isImm() )
		    pgm.cc.mov(mm, val.as<Imm>());
		else if ( val.isReg() && val.as<BaseReg>().isGroup(RegGroup::kVec) )
		{
		    if ( esize == sizeof(float) )
			pgm.cc.movss(mm, val.as<x86::Xmm>());
		    else
			pgm.cc.movsd(mm, val.as<x86::Xmm>());
		}
		else if ( val.isReg() )
		    pgm.cc.mov(mm, val.as<x86::Gp>());
		else if ( val.isMem() )
		{
		    x86::Gp tmp = pgm.cc.newGpq("flat_arr_tmp");
		    pgm.cc.mov(tmp, val.as<x86::Mem>());
		    pgm.cc.mov(mm, tmp);
		}
	    }
	    continue;
	}
	init_idx++;

	if ( !eff[i] )
	{
	    if ( i < dds->member_bitfields.size()
	      && dds->member_bitfields[i].is_bitfield )
	    {
		const DataDefSTRUCT::BitFieldInfo &bf = dds->member_bitfields[i];
		x86::Gp zero = pgm.cc.newGpq("init_bf_zero");
		pgm.cc.xor_(zero, zero);
		x86::Mem storage = x86::ptr(base_reg, addr, (uint32_t)bf.storage_size);
		emit_bitfield_store_reg(pgm, storage, bf, zero, "init_bf_zero");
	    }
	    else if ( mtype->is_numeric() || mtype->is_pointer() )
	    {
		x86::Mem mm = x86::ptr(base_reg, addr, (uint32_t)mtype->size);
		pgm.cc.mov(mm, imm(0));
	    }
	    else if ( mtype->basetype() == BaseType::btStruct )
	    {
		DataDefSTRUCT *ndds = dynamic_cast<DataDefSTRUCT *>(mtype);
		if ( ndds )
		{
		    std::vector<TokenBase *> empty_init;
		    emit_struct_init(pgm, base_reg, addr, ndds, empty_init, err_loc);
		}
	    }
	    continue;
	}

	if ( i < dds->member_bitfields.size()
	  && dds->member_bitfields[i].is_bitfield )
	{
	    regdefp_t bf_rdp = {nullptr, nullptr, nullptr};
	    bf_rdp.second = mtype;
	    Operand &bf_val = eff[i]->compile(pgm, bf_rdp);
	    const DataDefSTRUCT::BitFieldInfo &bf = dds->member_bitfields[i];
	    x86::Mem storage = x86::ptr(base_reg, addr, (uint32_t)bf.storage_size);
	    emit_bitfield_store_operand(pgm, storage, bf, bf_val,
		bf_rdp.second ? bf_rdp.second : eff[i]->datadef(),
		mtype, "init_bf");
	    continue;
	}

	std::string mname = mp.first;
	bool is_array_decl = dds->m_is_array_decl(mname) || member_count > 1;

	// Nested struct-member init: `{ ..., { 0, 1, 10 }, ... }` where
	// the member is itself a struct value. TokenStructLit has no
	// compile() — recurse here instead of letting it fall through to
	// TokenStmt's default-throw branch.
	if ( mtype->basetype() == BaseType::btStruct && !is_array_decl
	  && dynamic_cast<TokenStructLit *>(eff[i]) != NULL )
	{
	    TokenStructLit *nested = static_cast<TokenStructLit *>(eff[i]);
	    DataDefSTRUCT *ndds = dynamic_cast<DataDefSTRUCT *>(mtype);
	    if ( !ndds )
		pgm.Throw(err_loc) << "Nested initializer for non-struct member type" << flush;
	    emit_struct_init(pgm, base_reg, addr, ndds, nested->inits, err_loc);
	    continue;
	}
	if ( mtype->basetype() == BaseType::btStruct && !is_array_decl
	  && eff[i]->datadef()
	  && eff[i]->datadef()->basetype() == BaseType::btStruct )
	{
	    regdefp_t srdp = {nullptr, mtype, nullptr};
	    Operand &sval = eff[i]->compile(pgm, srdp);
	    Operand dst_slot = x86::ptr(base_reg, addr, (uint32_t)mtype->size);
	    emit_raw_aggregate_copy(pgm, dst_slot, sval, mtype, "init_struct_copy");
	    continue;
	}
	if ( mtype->basetype() == BaseType::btStruct && !is_array_decl )
	{
	    DataDefSTRUCT *ndds = dynamic_cast<DataDefSTRUCT *>(mtype);
	    if ( !ndds )
		pgm.Throw(err_loc) << "Nested initializer for non-struct member type" << flush;
	    size_t nested_slots = count_struct_init_slots(ndds);
	    std::vector<TokenBase *> flat_nested;
	    for ( size_t ni = i; ni < eff.size() && flat_nested.size() < nested_slots; ++ni )
		flat_nested.push_back(eff[ni]);
	    emit_struct_init(pgm, base_reg, addr, ndds, flat_nested, err_loc);
	    init_idx = i + flat_nested.size();
	    continue;
	}

	// Nested fixed-array member init: `{ ..., { 0, 1, 10 }, ... }`
	// where the member is `sh_int liq_affect[3]` — a fixed array of
	// the element type. Per-element write at `[base + addr + j*esize]`.
	// Member's per-member count lives on the parent struct.
	{
	    size_t mcount = dds->m_count(mname);
	    TokenStructLit *nested_arr = dynamic_cast<TokenStructLit *>(eff[i]);
	    if ( nested_arr != NULL && is_array_decl && mcount == 0 )
	    {
		if ( !nested_arr->inits.empty() )
		    pgm.Throw(err_loc) << "Too many initializers for member " << mname << flush;
		continue;
	    }
	    if ( nested_arr != NULL && is_array_decl && mcount == 1 )
	    {
		if ( nested_arr->inits.size() > 1 )
		    pgm.Throw(err_loc) << "Too many initializers for member " << mname << flush;
		if ( nested_arr->inits.empty() )
		{
		    x86::Mem mm = x86::ptr(base_reg, addr, (uint32_t)mtype->size);
		    pgm.cc.mov(mm, imm(0));
		    continue;
		}
		regdefp_t arr_rdp = {nullptr, nullptr, nullptr};
		Operand &elem_val = nested_arr->inits[0]->compile(pgm, arr_rdp);
		x86::Mem em = x86::ptr(base_reg, addr, (uint32_t)mtype->size);
		if ( elem_val.isImm() )
		    pgm.cc.mov(em, elem_val.as<Imm>());
		else if ( elem_val.isReg() && elem_val.as<BaseReg>().isGroup(RegGroup::kVec) )
		{
		    if ( mtype->size == sizeof(float) )
			pgm.cc.movss(em, elem_val.as<x86::Xmm>());
		    else
			pgm.cc.movsd(em, elem_val.as<x86::Xmm>());
		}
		else if ( elem_val.isReg() && elem_val.as<BaseReg>().isGroup(RegGroup::kGp) )
		{
		    x86::Gp src = elem_val.as<x86::Gp>();
		    if ( mtype->size == 8 ) pgm.cc.mov(em, src.r64());
		    else if ( mtype->size == 4 ) pgm.cc.mov(em, src.r32());
		    else if ( mtype->size == 2 ) pgm.cc.mov(em, src.r16());
		    else pgm.cc.mov(em, src.r8());
		}
		else
		    pgm.Throw(err_loc) << "Unsupported nested-array initializer element" << flush;
		continue;
	    }
	    if ( nested_arr != NULL && is_array_decl && mcount > 1 )
	    {
		size_t esize = mtype->size;
		std::vector<TokenBase *> elem_inits;
		for ( TokenBase *child : nested_arr->inits )
		{
		    if ( mtype->basetype() != BaseType::btStruct )
		    {
			if ( TokenStructLit *child_lit = dynamic_cast<TokenStructLit *>(child) )
			{
			    for ( TokenBase *grandchild : child_lit->inits )
				elem_inits.push_back(grandchild);
			    continue;
			}
		    }
		    elem_inits.push_back(child);
		}
		for ( size_t j = 0; j < elem_inits.size() && j < mcount; ++j )
		{
		    if ( mtype->basetype() == BaseType::btStruct )
		    {
			DataDefSTRUCT *elem_dds = dynamic_cast<DataDefSTRUCT *>(mtype);
			if ( !elem_dds )
			    pgm.Throw(err_loc) << "Nested initializer for non-struct array member type" << flush;
			if ( TokenStructLit *elem_slit = dynamic_cast<TokenStructLit *>(elem_inits[j]) )
			{
			    emit_struct_init(pgm, base_reg,
				addr + (int32_t)(j * esize), elem_dds, elem_slit->inits, err_loc);
			    continue;
			}
			if ( elem_inits[j]->datadef()
			  && elem_inits[j]->datadef()->basetype() == BaseType::btStruct )
			{
			    regdefp_t elem_rdp = {nullptr, mtype, nullptr};
			    Operand &elem_val = elem_inits[j]->compile(pgm, elem_rdp);
			    Operand dst_slot = x86::ptr(base_reg, addr + (int32_t)(j * esize), (uint32_t)esize);
			    emit_raw_aggregate_copy(pgm, dst_slot, elem_val, mtype, "init_struct_array_copy");
			    continue;
			}
		    }
		    regdefp_t arr_rdp = {nullptr, nullptr, nullptr};
		    Operand &elem_val = elem_inits[j]->compile(pgm, arr_rdp);
		    x86::Mem em = x86::ptr(base_reg, addr + (int32_t)(j * esize), (uint32_t)esize);
		    if ( elem_val.isImm() )
			pgm.cc.mov(em, elem_val.as<Imm>());
		    else if ( elem_val.isReg() && elem_val.as<BaseReg>().isGroup(RegGroup::kVec) )
		    {
			if ( esize == sizeof(float) )
			    pgm.cc.movss(em, elem_val.as<x86::Xmm>());
			else
			    pgm.cc.movsd(em, elem_val.as<x86::Xmm>());
		    }
		    else if ( elem_val.isReg() && elem_val.as<BaseReg>().isGroup(RegGroup::kGp) )
		    {
			x86::Gp src = elem_val.as<x86::Gp>();
			if ( esize == 8 ) pgm.cc.mov(em, src.r64());
			else if ( esize == 4 ) pgm.cc.mov(em, src.r32());
			else if ( esize == 2 ) pgm.cc.mov(em, src.r16());
			else pgm.cc.mov(em, src.r8());
		    }
		    else
			pgm.Throw(err_loc) << "Unsupported nested-array initializer element" << flush;
		}
		// Zero remaining slots
		for ( size_t j = elem_inits.size(); j < mcount; ++j )
		{
		    if ( mtype->basetype() == BaseType::btStruct )
		    {
			DataDefSTRUCT *elem_dds = dynamic_cast<DataDefSTRUCT *>(mtype);
			if ( elem_dds )
			{
			    std::vector<TokenBase *> empty_init;
			    emit_struct_init(pgm, base_reg,
				addr + (int32_t)(j * esize), elem_dds, empty_init, err_loc);
			    continue;
			}
		    }
		    x86::Mem em = x86::ptr(base_reg, addr + (int32_t)(j * esize), (uint32_t)esize);
		    pgm.cc.mov(em, imm(0));
		}
		continue;
	    }
	}

	regdefp_t it_rdp = {nullptr, mtype->is_real() ? mtype : nullptr, nullptr};
	Operand &val_op = eff[i]->compile(pgm, it_rdp);

	if ( mtype->is_numeric() || mtype->is_pointer() )
	{
	    // NOTE: do NOT mutate `val_op` directly — it's a reference to the
	    // operand stored in operand_map for global literal variables. Any
	    // reassignment would corrupt the cached entry for subsequent uses
	    // of the same literal. Use a local copy for the effective value.
	    Operand eff_val = val_op;

	    // char* member initialized from a string literal: coerce std::string → const char*
	    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(mtype);
	    if ( pdd && pdd->base_type == &ddCHAR
	      && it_rdp.second && it_rdp.second->is_string()
	      && val_op.isReg() )
	    {
		DBG(pgm.cc.comment("struct init: coerce string literal -> char*"));
		x86::Gp cstr_gp = pgm.cc.newIntPtr("_si_cstr");
		InvokeNode *cstr_call;
		pgm.cc.invoke(&cstr_call, imm(string_cstr),
		    FuncSignature::build<const char *, void *>());
		cstr_call->setArg(0, as_gp_ptr(pgm, val_op, "_si_strptr"));
		cstr_call->setRet(0, cstr_gp);
		eff_val = cstr_gp;
	    }

	    size_t esize = mtype->size;
	    x86::Mem mm = x86::ptr(base_reg, addr, (uint32_t)esize);
	    if ( eff_val.isImm() )
		pgm.cc.mov(mm, eff_val.as<Imm>());
	    else if ( eff_val.isReg() && eff_val.as<BaseReg>().isGroup(RegGroup::kVec) )
	    {
		if ( esize == sizeof(float) )
		    pgm.cc.movss(mm, eff_val.as<x86::Xmm>());
		else
		    pgm.cc.movsd(mm, eff_val.as<x86::Xmm>());
	    }
	    else if ( eff_val.isReg() )
	    {
		x86::Gp vgp = eff_val.as<x86::Gp>();
		if      ( esize == 8 ) pgm.cc.mov(mm, vgp.r64());
		else if ( esize == 4 ) pgm.cc.mov(mm, vgp.r32());
		else if ( esize == 2 ) pgm.cc.mov(mm, vgp.r16());
		else                   pgm.cc.mov(mm, vgp.r8());
	    }
	    else if ( eff_val.isMem() )
	    {
		x86::Gp tmp = pgm.cc.newGpq("_si_tmp");
		pgm.cc.mov(tmp, eff_val.as<x86::Mem>());
		if      ( esize == 8 ) pgm.cc.mov(mm, tmp.r64());
		else if ( esize == 4 ) pgm.cc.mov(mm, tmp.r32());
		else if ( esize == 2 ) pgm.cc.mov(mm, tmp.r16());
		else                   pgm.cc.mov(mm, tmp.r8());
	    }
	}
	else if ( mtype->is_string() )
	{
	    x86::Gp m_addr = pgm.cc.newIntPtr("_si_straddr");
	    pgm.cc.lea(m_addr, x86::ptr(base_reg, addr));
	    if ( val_op.isReg() )
	    {
		InvokeNode *call;
		pgm.cc.invoke(&call, imm(string_assign),
		    FuncSignature::build<void, void *, void *>());
		call->setArg(0, m_addr);
		call->setArg(1, as_gp_ptr(pgm, val_op, "str_val"));
	    }
	}
	else
	{
	    pgm.Throw(err_loc) << "Unsupported struct member type in initializer: " << mtype->name << flush;
	}
    }
    if ( dds->union_layout )
	return;
    // Zero-fill remaining numeric/pointer members
    for ( size_t i = eff.size(); i < dds->members.size(); ++i )
    {
	auto &mp = dds->members[i];
	DataDef *mtype = mp.second;
	int32_t addr = base_ofs + (int32_t)((i < dds->member_offsets.size())
	    ? dds->member_offsets[i] : 0);
	if ( i < dds->member_bitfields.size()
	  && dds->member_bitfields[i].is_bitfield )
	{
	    const DataDefSTRUCT::BitFieldInfo &bf = dds->member_bitfields[i];
	    x86::Gp zero = pgm.cc.newGpq("init_bf_zero");
	    pgm.cc.xor_(zero, zero);
	    x86::Mem storage = x86::ptr(base_reg, addr, (uint32_t)bf.storage_size);
	    emit_bitfield_store_reg(pgm, storage, bf, zero, "init_bf_zero");
	    continue;
	}
	if ( mtype->is_numeric() || mtype->is_pointer() )
	{
	    x86::Mem mm = x86::ptr(base_reg, addr, (uint32_t)mtype->size);
	    pgm.cc.mov(mm, imm(0));
	}
	else if ( mtype->basetype() == BaseType::btStruct )
	{
	    DataDefSTRUCT *ndds = dynamic_cast<DataDefSTRUCT *>(mtype);
	    if ( ndds )
	    {
		std::vector<TokenBase *> empty_init;
		emit_struct_init(pgm, base_reg, addr, ndds, empty_init, err_loc);
	    }
	}
    }
}

Operand &TokenDecl::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDecl::compile(" << var.name << " regdp.second: " << (regdp.second ? regdp.second->name : "")<<  ") TOP" << endl);
    bool emit_initializer = !(var.flags & vfSTATIC) || pgm.tkFunction == pgm.tkProgram;

    // Local statics with brace-init lists (struct/array initializers) still
    // need their init code emitted — static placement only zeroes the memory.
    // Emit init unconditionally (static vars are only initialized once since
    // they live in the data section — the function is only JIT'd once).
    if ( !emit_initializer && !init_list.empty() )
	emit_initializer = true;

    // VLAs need their malloc-init emitted at the declaration point — not
    // lazily on first use. Lazy emission would put the malloc inside the
    // enclosing loop body if the first subscript appears in a loop, and
    // every iteration would reallocate (wiping prior writes). Touch
    // voperand now so the buffer is allocated exactly once at decl time.
    if ( var.is_vla() )
	pgm.tkFunction->voperand(pgm, &var);
    if ( aggregate_has_runtime_size(var.type) )
	pgm.tkFunction->voperand(pgm, &var);
    // Class objects with user-defined constructors must be allocated and
    // constructed at declaration time, not lazily on first use.
    if ( var.type->basetype() == BaseType::btClass )
    {
	DataDefCLASS *ddc = static_cast<DataDefCLASS *>(var.type);
	if ( ddc->has_user_ctor || ddc->has_user_dtor )
	{
	    Operand &obj_op = pgm.tkFunction->voperand(pgm, &var);
	    // Call base class constructor first (inheritance)
	    if ( ddc->base_class && ddc->base_class->has_user_ctor )
	    {
		std::string base_ctor_name = ddc->base_class->name;
		Variable *base_ctor_mvar = ddc->base_class->findMethod(base_ctor_name);
		if ( base_ctor_mvar && base_ctor_mvar->data )
		{
		    FuncDef *base_ctor = (FuncDef *)((Method *)base_ctor_mvar->data)->returns.type;
		    if ( base_ctor && base_ctor->funcnode )
		    {
			DBG(pgm.cc.comment("base class constructor call"));
			x86::Gp base_this = pgm.cc.newIntPtr("__base_ctor_this");
			if ( obj_op.isMem() )
			    pgm.cc.lea(base_this, obj_op.as<x86::Mem>());
			else
			    pgm.cc.mov(base_this, obj_op.as<x86::Gp>());
			InvokeNode *bcall;
			pgm.cc.invoke(&bcall, base_ctor->funcnode->label(),
				      FuncSignature::build<void, void *>());
			bcall->setArg(0, base_this);
		    }
		}
	    }
	    // Call user-defined constructor
	    if ( ddc->has_user_ctor )
	    {
		std::string ctor_name = ddc->name;
		Variable *ctor_mvar = ddc->findMethod(ctor_name);
		if ( ctor_mvar && ctor_mvar->data )
		{
		    FuncDef *ctor_func = (FuncDef *)((Method *)ctor_mvar->data)->returns.type;
		    if ( ctor_func && ctor_func->funcnode )
		    {
			DBG(pgm.cc.comment("user constructor call"));
			// Build function signature: void(this, arg1, arg2, ...)
			FuncSignature funcsig;
			funcsig.setCallConvId(CallConvId::kCDecl);
			funcsig.setRetT<void>();
			funcsig.addArgT<void *>(); // __this
			// Compile constructor arguments and build params
			std::vector<Operand> arg_ops;
			for ( size_t i = 0; i < ctor_args.size(); ++i )
			{
			    regdefp_t arg_regdp = {NULL, NULL, NULL};
			    Operand &arg_result = ctor_args[i]->compile(pgm, arg_regdp);
			    // Determine the argument type from the ctor's FuncDef
			    DataDef *arg_type = NULL;
			    // +1 to skip __this parameter
			    if ( i + 1 < ctor_func->parameters.size() )
				arg_type = ctor_func->parameters[i + 1];
			    add_funcsig_arg(funcsig, arg_type);
			    arg_ops.push_back(arg_result);
			}
			// Emit the invoke
			x86::Gp this_ptr = pgm.cc.newIntPtr("__ctor_this");
			if ( obj_op.isMem() )
			    pgm.cc.lea(this_ptr, obj_op.as<x86::Mem>());
			else
			    pgm.cc.mov(this_ptr, obj_op.as<x86::Gp>());
			InvokeNode *call;
			pgm.cc.invoke(&call, ctor_func->funcnode->label(), funcsig);
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
				x86::Gp tmp = pgm.cc.newIntPtr("__ctor_arg");
				pgm.cc.mov(tmp, arg_ops[i].as<x86::Mem>());
				call->setArg(i + 1, tmp);
			    }
			}
		    }
		}
	    }
	}
    }

    // Top-level program declarations still need their initializer code emitted
    // so file-scope C globals like `static char *p = "x";` and static tables
    // are materialized before `main()` runs. Only local statics skip here.
    if ( initialize
      && emit_initializer
      && !global_string_initializer_is_static_data(pgm, var, initialize) )
	initialize->compile(pgm, regdp);

    // Empty brace initializer for structs/unions: `struct X x = {};`
    // Zero-fill the stack slot directly instead of going through the
    // assignment path, which would self-copy (source == dest).
    if ( emit_initializer
      && init_list.empty() && has_brace_init && !var.is_fixed_array()
      && dynamic_cast<DataDefSTRUCT *>(var.type) != NULL
      && var.type->type() == DataType::dtRESERVED )
    {
	Operand &base_op = pgm.tkFunction->voperand(pgm, &var);
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", (var.name + ".zero").c_str());
	if ( base_op.isMem() )
	    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
	else
	    pgm.cc.mov(base_reg, base_op.as<x86::Gp>());
	size_t total = var.type->size;
	// Zero-fill using qword stores, then any remainder.
	size_t ofs = 0;
	for ( ; ofs + 8 <= total; ofs += 8 )
	    pgm.cc.mov(x86::qword_ptr(base_reg, (int32_t)ofs), imm(0));
	for ( ; ofs + 4 <= total; ofs += 4 )
	    pgm.cc.mov(x86::dword_ptr(base_reg, (int32_t)ofs), imm(0));
	for ( ; ofs < total; ofs++ )
	    pgm.cc.mov(x86::byte_ptr(base_reg, (int32_t)ofs), imm(0));
	regdp.first = &base_op;
	regdp.second = var.type;
    }

    // brace-enclosed initializer for standalone structs: struct Foo x = { v0, v1, ... }
    if ( emit_initializer
      && !init_list.empty() && !var.is_fixed_array()
      && dynamic_cast<DataDefSTRUCT *>(var.type) != NULL
      && var.type->type() == DataType::dtRESERVED )
    {
	DBG(pgm.cc.comment("TokenDecl struct init_list"));
	DataDefSTRUCT *dds = static_cast<DataDefSTRUCT *>(var.type);
	// Allow flat initialization: `struct { int f[4]; } s = {1,2,3,4};`
	// Count total scalar slots (expanding fixed arrays) for the check.
	{
	    size_t total_slots = count_struct_init_slots(dds);
	    size_t used_slots = count_initializer_entries(init_list);
	    if ( used_slots > total_slots )
		pgm.Throw(this) << "Too many initializers for struct " << dds->name << flush;
	}

	Operand &base_op = pgm.tkFunction->voperand(pgm, &var);
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", (var.name + ".init_base").c_str());
	if ( base_op.isMem() )
	    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
	else
	    pgm.cc.mov(base_reg, base_op.as<x86::Gp>());

	emit_struct_init(pgm, base_reg, 0, dds, init_list, this);
    }

    // brace-enclosed initializer for standalone SIMD vectors:
    // V2SI x = { 2, 2 };
    if ( emit_initializer
      && !init_list.empty() && !var.is_fixed_array()
      && var.type && var.type->is_simd() )
    {
	DBG(pgm.cc.comment("TokenDecl simd init_list"));
	DataDefSIMD *vdd = static_cast<DataDefSIMD *>(var.type);
	Operand &base_op = pgm.tkFunction->voperand(pgm, &var);
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", (var.name + ".simd_init_base").c_str());
	if ( base_op.isMem() )
	    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
	else
	    pgm.cc.mov(base_reg, base_op.as<x86::Gp>());

	emit_zero_fill_region(pgm, base_reg, 0, var.type->size);
	emit_simd_init(pgm, base_reg, 0, vdd, init_list, this);
    }

    // brace-enclosed initializer for fixed-size arrays: arr[N] = { v0, v1, ... }
    if ( emit_initializer && !init_list.empty() && var.is_fixed_array() )
    {
	DBG(pgm.cc.comment("TokenDecl fixed-array init_list"));
	Operand &base_op = pgm.tkFunction->voperand(pgm, &var);
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", (var.name + ".init_base").c_str());
	pgm.cc.mov(base_reg, base_op.as<x86::Gp>());

	// Array-of-structs: each element is a nested brace list; emit member
	// writes at [base + i*struct_size + member_offset] per element.
	DataDefSTRUCT *elem_dds =
	    (var.type->type() == DataType::dtRESERVED)
	    ? dynamic_cast<DataDefSTRUCT *>(var.type) : NULL;
	if ( elem_dds )
	{
	    size_t struct_size = var.type->size;
	    for ( size_t i = 0; i < init_list.size(); ++i )
	    {
		TokenStructLit *slit = dynamic_cast<TokenStructLit *>(init_list[i]);
		if ( slit )
		{
		    emit_struct_init(pgm, base_reg, (int32_t)(i * struct_size),
			elem_dds, slit->inits, this);
		    continue;
		}
		std::vector<TokenBase *> scalar_init;
		scalar_init.push_back(init_list[i]);
		emit_struct_init(pgm, base_reg, (int32_t)(i * struct_size),
		    elem_dds, scalar_init, this);
	    }
	    // Zero-fill remaining element slots byte by byte (8 bytes at a time)
	    uint32_t total = var.total_elements();
	    for ( size_t i = init_list.size(); i < total; ++i )
	    {
		for ( size_t off = 0; off < struct_size; off += 8 )
		{
		    size_t chunk = (struct_size - off >= 8) ? 8 :
		                   (struct_size - off >= 4) ? 4 :
		                   (struct_size - off >= 2) ? 2 : 1;
		    x86::Mem slot = x86::ptr(base_reg,
			(int32_t)(i * struct_size + off), (uint32_t)chunk);
		    pgm.cc.mov(slot, imm(0));
		}
	    }
	    DBG(cout << "TokenDecl::compile(" << var.name << ") END" << endl);
	    return _reg;
	}

	std::vector<TokenBase *> flat_inits;
	std::function<void(TokenBase *)> flatten_init = [&](TokenBase *node)
	{
	    if ( TokenStructLit *slit = dynamic_cast<TokenStructLit *>(node) )
	    {
		for ( TokenBase *child : slit->inits )
		    flatten_init(child);
		return;
	    }
	    flat_inits.push_back(node);
	};
	for ( TokenBase *node : init_list )
	    flatten_init(node);

	size_t elem_size = var.type->size ? var.type->size : 8;
	bool elem_is_charptr = type_is_cstr_pointer(var.type);
	Variable *static_data_var = &var;
	if ( pgm.tkProgram && var.is_global() )
	{
	    Variable *canon = pgm.tkProgram->findVariable(var.name);
	    if ( canon && canon->data )
		static_data_var = canon;
	}
	for ( size_t i = 0; i < flat_inits.size(); ++i )
	{
	    const char *static_cstr_addr = NULL;
	    if ( pgm.tkFunction == pgm.tkProgram
	      && static_data_var && static_data_var->data
	      && var.type
	      && var.type->is_pointer()
	      && token_get_constant_cstring_address(flat_inits[i], static_cstr_addr) )
	    {
		memcpy(static_cast<uint8_t *>(static_data_var->data) + i * elem_size,
		       &static_cstr_addr, sizeof(static_cstr_addr));
		continue;
	    }

	    regdefp_t it_rdp = {nullptr, var.type->is_numeric() ? var.type : nullptr, nullptr};
	    Operand &val_op = flat_inits[i]->compile(pgm, it_rdp);
	    x86::Mem slot = x86::ptr(base_reg, (int32_t)(i * elem_size), (uint32_t)elem_size);
	    // char *arr[] = {"alice", ...}: each string-literal init yields a
	    // std::string object pointer; coerce to c_str() so the slot
	    // holds a real char * instead of the object address.
	    if ( elem_is_charptr && flat_inits[i]->datadef()
	      && flat_inits[i]->datadef()->rawtype() == DataType::dtSTRING
	      && val_op.isReg() )
	    {
		x86::Gp cstr = pgm.cc.newIntPtr("init_cstr");
		InvokeNode *cstr_call;
		pgm.cc.invoke(&cstr_call, imm(string_cstr),
		    FuncSignature::build<const char *, void *>());
		cstr_call->setArg(0, as_gp_ptr(pgm, val_op, "_si_strptr"));
		cstr_call->setRet(0, cstr);
		pgm.cc.mov(slot, cstr);
		continue;
	    }
	    if ( val_op.isImm() )
		pgm.cc.mov(slot, val_op.as<Imm>());
	    else if ( val_op.isReg() && val_op.as<BaseReg>().isGroup(RegGroup::kVec) )
	    {
		if ( elem_size == sizeof(float) )
		    pgm.cc.movss(slot, val_op.as<x86::Xmm>());
		else
		    pgm.cc.movsd(slot, val_op.as<x86::Xmm>());
	    }
	    else if ( val_op.isReg() )
	    {
		x86::Gp vgp = val_op.as<x86::Gp>();
		if      ( elem_size == 8 ) pgm.cc.mov(slot, vgp.r64());
		else if ( elem_size == 4 ) pgm.cc.mov(slot, vgp.r32());
		else if ( elem_size == 2 ) pgm.cc.mov(slot, vgp.r16());
		else                       pgm.cc.mov(slot, vgp.r8());
	    }
	    else if ( val_op.isMem() )
	    {
		x86::Gp tmp = pgm.cc.newGpq("_init_tmp");
		pgm.cc.mov(tmp, val_op.as<x86::Mem>());
		if      ( elem_size == 8 ) pgm.cc.mov(slot, tmp.r64());
		else if ( elem_size == 4 ) pgm.cc.mov(slot, tmp.r32());
		else if ( elem_size == 2 ) pgm.cc.mov(slot, tmp.r16());
		else                       pgm.cc.mov(slot, tmp.r8());
	    }
	}
	// zero-fill remaining slots (C initializer semantics)
	uint32_t total = var.total_elements();
	for ( size_t i = flat_inits.size(); i < total; ++i )
	{
	    x86::Mem slot = x86::ptr(base_reg, (int32_t)(i * elem_size), (uint32_t)elem_size);
	    pgm.cc.mov(slot, imm(0));
	}
    }

    DBG(cout << "TokenDecl::compile(" << var.name << ") END" << endl);

    return _reg;
}

// new ClassName(args) — malloc + constructor call, return pointer
Operand &TokenNEW::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenNEW::compile()"));
    if ( !alloc_class )
	throw "TokenNEW::compile() missing class type";

    // Allocate memory: malloc(class_size)
    x86::Gp obj_ptr = pgm.cc.newIntPtr("__new_obj");
    x86::Gp size_reg = pgm.cc.newGpq("__new_size");
    pgm.cc.mov(size_reg, imm((int64_t)alloc_class->size));
    InvokeNode *mcall;
    pgm.cc.invoke(&mcall, imm((void *)::malloc),
		  FuncSignature::build<void *, size_t>());
    mcall->setArg(0, size_reg);
    mcall->setRet(0, obj_ptr);

    // Call constructor if the class has one
    if ( alloc_class->has_user_ctor )
    {
	std::string ctor_name = alloc_class->name;
	Variable *ctor_mvar = alloc_class->findMethod(ctor_name);
	if ( ctor_mvar && ctor_mvar->data )
	{
	    FuncDef *ctor_func = (FuncDef *)((Method *)ctor_mvar->data)->returns.type;
	    if ( ctor_func && ctor_func->funcnode )
	    {
		DBG(pgm.cc.comment("new: constructor call"));
		FuncSignature funcsig;
		funcsig.setCallConvId(CallConvId::kCDecl);
		funcsig.setRetT<void>();
		funcsig.addArgT<void *>(); // __this

		std::vector<Operand> arg_ops;
		for ( size_t i = 0; i < ctor_args.size(); ++i )
		{
		    regdefp_t arg_regdp = {NULL, NULL, NULL};
		    Operand &arg_result = ctor_args[i]->compile(pgm, arg_regdp);
		    DataDef *arg_type = (i + 1 < ctor_func->parameters.size())
			? ctor_func->parameters[i + 1] : NULL;
		    add_funcsig_arg(funcsig, arg_type);
		    arg_ops.push_back(arg_result);
		}

		InvokeNode *ccall;
		pgm.cc.invoke(&ccall, ctor_func->funcnode->label(), funcsig);
		ccall->setArg(0, obj_ptr);
		for ( size_t i = 0; i < arg_ops.size(); ++i )
		{
		    if ( arg_ops[i].isReg() )
		    {
			if ( arg_ops[i].as<BaseReg>().isGroup(RegGroup::kVec) )
			    ccall->setArg(i + 1, arg_ops[i].as<x86::Xmm>());
			else
			    ccall->setArg(i + 1, arg_ops[i].as<x86::Gp>());
		    }
		    else if ( arg_ops[i].isMem() )
		    {
			x86::Gp tmp = pgm.cc.newIntPtr("__new_arg");
			pgm.cc.mov(tmp, arg_ops[i].as<x86::Mem>());
			ccall->setArg(i + 1, tmp);
		    }
		}
	    }
	}
    }

    // If caller provided a destination (assignment context), store there
    if ( regdp.first )
    {
	if ( regdp.first->isMem() )
	    pgm.cc.mov(regdp.first->as<x86::Mem>(), obj_ptr);
	else if ( regdp.first->isReg() )
	    pgm.cc.mov(regdp.first->as<x86::Gp>(), obj_ptr);
	regdp.second = pgm.getPointerType(alloc_class);
	return *regdp.first;
    }
    _operand = obj_ptr;
    regdp.first = &_operand;
    regdp.second = pgm.getPointerType(alloc_class);
    return _operand;
}

// delete expr — destructor call + free
Operand &TokenDELETE::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenDELETE::compile()"));
    if ( !expr )
	throw "TokenDELETE::compile() missing expression";

    // Compile the expression to get the pointer
    regdefp_t erd = {NULL, NULL, NULL};
    Operand &ptr_op = expr->compile(pgm, erd);
    x86::Gp obj_ptr = pgm.cc.newIntPtr("__del_ptr");
    if ( ptr_op.isMem() )
	pgm.cc.mov(obj_ptr, ptr_op.as<x86::Mem>());
    else if ( ptr_op.isReg() )
	pgm.cc.mov(obj_ptr, ptr_op.as<x86::Gp>());

    // Call destructor if the class has one
    if ( del_class && del_class->has_user_dtor )
    {
	std::string dtor_name = "~" + del_class->name;
	Variable *dtor_mvar = del_class->findMethod(dtor_name);
	if ( dtor_mvar && dtor_mvar->data )
	{
	    FuncDef *dtor_func = (FuncDef *)((Method *)dtor_mvar->data)->returns.type;
	    if ( dtor_func && dtor_func->funcnode )
	    {
		DBG(pgm.cc.comment("delete: destructor call"));
		InvokeNode *dcall;
		pgm.cc.invoke(&dcall, dtor_func->funcnode->label(),
			      FuncSignature::build<void, void *>());
		dcall->setArg(0, obj_ptr);
	    }
	}
    }

    // Free the memory
    DBG(pgm.cc.comment("delete: free()"));
    InvokeNode *fcall;
    pgm.cc.invoke(&fcall, imm((void *)::free),
		  FuncSignature::build<void, void *>());
    fcall->setArg(0, obj_ptr);

    _operand = obj_ptr;
    return _operand;
}

void TokenFunc::prepareFuncNode(Program &pgm)
{
    if ( !var.data ) return; // not a real function (edge case)
    Method &method = *((Method *)var.data);
    FuncDef *func = (FuncDef *)method.returns.type;
    if ( pgm.instrument_functions )
	std::cerr << "[instrument] " << var.name << " no_instrument="
		  << (func->no_instrument_function ? 1 : 0) << std::endl;
    if ( func->funcnode ) return; // already prepared

    FuncSignature funcsig(CallConvId::kCDecl);
    datadef_vec_iter dvi;

    // multi-return: inject hidden __retbuf param if not already present
    if ( func->is_multi_return() )
    {
	bool has_retbuf = false;
	for ( auto *p : method.parameters )
	    if ( p->name == "__retbuf" ) { has_retbuf = true; break; }
	if ( !has_retbuf )
	{
	    std::string rbname = "__retbuf";
	    Variable *rbvar = new Variable(rbname, ddINT64, 1, NULL, false);
	    rbvar->flags |= vfPARAM;
	    method.parameters.insert(method.parameters.begin(), rbvar);
	    func->parameters.insert(func->parameters.begin(), &ddINT64);
	}
    }

    // Large struct return (>16 bytes): inject hidden __retbuf param.
    // Per System V AMD64 ABI, caller allocates buffer and passes its
    // address as the first argument; callee copies into it.
    if ( !func->is_multi_return() && is_large_struct_return(&func->returns) )
    {
	bool has_retbuf = false;
	for ( auto *p : method.parameters )
	    if ( p->name == "__retbuf" ) { has_retbuf = true; break; }
	if ( !has_retbuf )
	{
	    std::string rbname = "__retbuf";
	    Variable *rbvar = new Variable(rbname, ddINT64, 1, NULL, false);
	    rbvar->flags |= vfPARAM;
	    method.parameters.insert(method.parameters.begin(), rbvar);
	    func->parameters.insert(func->parameters.begin(), &ddINT64);
	    func->has_large_struct_retbuf = true;
	}
    }

    // varargs: inject hidden __va_args param if not already present
    if ( func->is_varargs )
    {
	bool has_va = false;
	for ( auto *p : method.parameters )
	    if ( p->name == "__va_args" ) { has_va = true; break; }
	if ( !has_va )
	{
	    std::string vaname = "__va_args";
	    Variable *vavar = new Variable(vaname, ddINT64, 1, NULL, false);
	    vavar->flags |= vfPARAM;
	    method.parameters.push_back(vavar);
	    func->parameters.push_back(&ddINT64);
	}
    }

    set_funcsig_ret(funcsig, &func->returns,
	func->is_multi_return() || is_large_struct_return(&func->returns));

    // set parameter types
    for ( dvi = func->parameters.begin(); dvi != func->parameters.end(); ++dvi )
	add_funcsig_arg(funcsig, *dvi);

    if ( !(func->funcnode=pgm.cc.newFunc(funcsig)) )
    {
	std::cerr << "Failed to create funcnode!" << std::endl;
	throw "Failed to create funcnode";
    }
}

Operand &TokenFunc::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenFunc::compile(" << var.name << '[' << (uint64_t)this << "]) TOP" << endl);
    // Earlier duplicate definition — a later TokenFunc with the same
    // FuncDef will compile the winning body. Skip silently here so we
    // don't double-addFunc the shared funcnode.
    if ( is_overridden ) return _operand;
    if ( !var.data ) { throw "TokenFunc::compile: method is NULL"; }

    Method &method = *((Method *)var.data);
    FuncDef *func = (FuncDef *)method.returns.type;
    // Pre-pass in Program::compile usually already prepared this function's
    // funcnode so global fn-pointer inits could LEA its label. If not
    // (e.g. a late-compiled lambda), do it now.
    prepareFuncNode(pgm);

    pgm.tkFunction = this;
    pgm.cur_func_name = var.name; // for asmjit ErrorHandler context
    clear_operand_map(); // clear operand map
    pgm.label_map.clear(); // labels are function-scoped

    pgm.cc.addFunc(func->funcnode);
    // Function-entry anchor — covers the prologue / param-binding so
    // crashes there resolve to the function's source location instead
    // of falling through to the previous function's last anchor.
    if ( const char *e = ::getenv("MADC_TRACE_FNENTRY") )
	if ( e[0] ) std::cerr << "[fn-entry] " << var.name << " file="
			      << (this->file ? this->file : "(null)")
			      << " line=" << this->line << std::endl;
    pgm.record_compile_anchor(this, "fn-entry");

    if ( method.parameters.size() )
    {
	DBG(cout << "TokenFunc::compile() has parameters:" << endl);
	uint32_t argc = 0;

	for ( variable_vec_iter vvi = method.parameters.begin(); vvi != method.parameters.end(); ++vvi )
	{
	    DBG(std::cout << "TokenFunc::compile(): funcnode->setArg(" << argc << ", " << (*vvi)->name << ')' << std::endl);
	    Operand &reg = voperand(pgm, (*vvi));

	    if ( reg.isReg() )
	    {
		if ( reg.as<BaseReg>().isGroup(RegGroup::kVec) )
		    func->funcnode->setArg(argc++, reg.as<x86::Xmm>());
		else
		if ( reg.as<BaseReg>().isGroup(RegGroup::kGp) )
		    func->funcnode->setArg(argc++, reg.as<x86::Gp>());
		else
		    throw "TokenFunc::compile() unexpected parameter Operand";
	    }
	    else
	    if ( reg.isMem() )
	    {
		Operand tmp = (*vvi)->type->newreg(pgm.cc, (*vvi)->name.c_str());
		if ( tmp.isReg() && tmp.as<BaseReg>().isGroup(RegGroup::kVec) )
		{
		    func->funcnode->setArg(argc++, tmp.as<x86::Xmm>());
		    x86::Mem dst = reg.as<x86::Mem>();
		    if ( (*vvi)->type && (*vvi)->type->is_simd() )
			store_xmm_to_mem(pgm, dst, tmp.as<x86::Xmm>(), (*vvi)->type);
		    else
		    {
			IRBuilder ir(pgm.cc);
			ir.store(IRValue::mem(dst, (*vvi)->type),
				 ir_from_operand(tmp, (*vvi)->type));
		    }
		}
		else
		if ( tmp.isReg() && tmp.as<BaseReg>().isGroup(RegGroup::kGp) )
		{
		    func->funcnode->setArg(argc++, tmp.as<x86::Gp>());
		    IRBuilder ir(pgm.cc);
		    ir.store(IRValue::mem(reg.as<x86::Mem>(), (*vvi)->type),
			     ir_from_operand(tmp, (*vvi)->type));
		}
		else
		    throw "TokenFunc::compile() unexpected stack parameter Operand";
	    }
	    else
		throw "TokenFunc::compile() argument not register";

	    (*vvi)->flags |= vfREGSET;
	}
    }

    for ( variable_vec_iter vvi = method.parameters.begin();
	  vvi != method.parameters.end(); ++vvi )
    {
	if ( !(*vvi)->param_vla_side_effect_expr )
	    continue;
	regdefp_t sidefx_rdp = {NULL, NULL, NULL};
	(*vvi)->param_vla_side_effect_expr->compile(pgm, sidefx_rdp);
    }

    emit_function_instrument_enter(pgm, func);

    if ( variables.size() )
    {
	DBG(cout << "Local variables:" << endl);
	for ( variable_vec_iter vvi = variables.begin(); vvi != variables.end(); ++vvi )
	{
	    DBG(cout << "    " << (*vvi)->type->name << ' ' << (*vvi)->name << endl);
	}
    }

    // Save the cursor position right after parameter setup. Local
    // numeric stack-slot zero-inits will be inserted here (via
    // setCursor) so they execute once at function entry instead of at
    // first-use, which could be inside a conditional branch.
    prologue_cursor = pgm.cc.cursor();

    auto invalidate_rematerializable_globals = [&]() {
	for ( std::map<Variable *, Operand>::iterator it = operand_map.begin();
	      it != operand_map.end(); )
	{
	    Variable *var = it->first;
	    if ( var && var->is_global() && (var->data || var->has_aot_data())
	      && (var->is_fixed_array()
	       || var->type->basetype() == BaseType::btStruct
	       || var->type->basetype() == BaseType::btClass) )
	    {
		it = operand_map.erase(it);
		continue;
	    }
	    ++it;
	}
    };

    for ( vector<TokenStmt *>::iterator si = statements.begin(); si != statements.end(); ++si )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	pgm.record_compile_anchor(*si, "stmt");
	(*si)->compile(pgm, regdp);
	invalidate_rematerializable_globals();
    }

    cleanup(pgm);	// cleanup stack
    emit_function_instrument_exit(pgm, func);
    if ( var.name == "main" && !method.owner_class
      && func->returns.rawtype() != DataType::dtVOID )
    {
	x86::Gp ret0 = pgm.cc.newGpq("__main_ret0");
	pgm.cc.xor_(ret0, ret0);
	pgm.cc.ret(ret0);
    }
    else
    if ( func->returns.rawtype() == DataType::dtVOID && !func->returns.is_pointer() )
	emit_zeroed_void_return(pgm);
    else
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
    TokenBase *prepass_tb = NULL;
    regdefp_t regdp = {NULL, NULL, NULL};

    DBG(cout << endl << endl << "Program::compile() start" << endl << endl);
    clear_diagnostics();
    clear_error();
    _compiler_init();
    if ( aot_tracking )
	prepare_aot_data_layout();

    // Dedupe pre-pass: when the same function is defined more than once
    // (e.g. shim stubs that override an upstream definition), every
    // TokenFunc shares one FuncDef + funcnode. Calling cc.addFunc(node)
    // twice for the same FuncNode confuses asmjit's Compiler — bodies of
    // every funcnode added between the duplicate calls end up with
    // unbound labels. Walk pending_funcs in reverse, keep only the last
    // TokenFunc per FuncDef, mark earlier duplicates as overridden so
    // both prepareFuncNode and TokenFunc::compile skip them.
    {
	std::set<FuncDef *> seen_fd;
	for ( auto it = pending_funcs.rbegin(); it != pending_funcs.rend(); ++it )
	{
	    TokenFunc *tf = dynamic_cast<TokenFunc *>(*it);
	    if ( !tf || !tf->var.data ) continue;
	    Method *m = (Method *)tf->var.data;
	    FuncDef *fd = dynamic_cast<FuncDef *>(m->returns.type);
	    if ( !fd ) continue;
	    if ( seen_fd.count(fd) )
		tf->is_overridden = true;
	    else
		seen_fd.insert(fd);
	}
    }

    // Pre-pass: create FuncNode labels for every user-defined function and
    // lambda so global fn-pointer inits (which compile first via TokenProgram)
    // can LEA the target label. The bodies still compile in the normal loop.
    try
    {
	for ( TokenBase *tb_func : pending_funcs )
	{
	    prepass_tb = tb_func;
	    TokenFunc *tf = dynamic_cast<TokenFunc *>(tb_func);
	    if ( tf && !tf->is_overridden ) tf->prepareFuncNode(*this);
	}
    }
    catch(const char *err_msg)
    {
	set_error(Program::DiagnosticPhase::compiler, err_msg ? err_msg : "(null error message)",
	    prepass_tb && prepass_tb->file ? prepass_tb->file : NULL,
	    prepass_tb ? prepass_tb->line : 0,
	    prepass_tb ? prepass_tb->column : 0);
	print_last_diagnostic(error(), "(during funcnode pre-pass)");
	return false;
    }

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
	set_error(Program::DiagnosticPhase::compiler, err_msg ? err_msg : "(null error message)",
	    tb && tb->file ? tb->file : NULL,
	    tb ? tb->line : 0,
	    tb ? tb->column : 0);
	print_last_diagnostic(error());
	return false;
    }
    catch(TokenBase *tb)
    {
	set_error(Program::DiagnosticPhase::compiler, std::string("unexpected token type ") + std::to_string((int)tb->type()),
	    tb && tb->file ? tb->file : NULL, tb ? tb->line : 0, tb ? tb->column : 0);
	print_last_diagnostic(error());
	return false;
    }
    catch(std::exception &e)
    {
	if ( !last_error.has_error )
	{
	    TokenBase *err_tb = Throw.token();
	    set_error(Program::DiagnosticPhase::compiler, Throw.str().empty() ? e.what() : Throw.str(),
		err_tb && err_tb->file ? err_tb->file : NULL,
		err_tb ? err_tb->line : 0,
		err_tb ? err_tb->column : 0);
	}
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

    clear_diagnostics();
    clear_error();

    push_runtime_scope();
    DBG(std::cout << "Program::execute() calling root_fn()" << std::endl);
    root_fn();

    if ( !var )
    {
	set_error(Program::DiagnosticPhase::runtime, "Program::execute() cannot find main");
	DBG(error() << "Program::execute() cannot find main" << std::endl);
	print_last_diagnostic(error());
	pop_runtime_scope();
	return;
    }
    if ( var->type->basetype() != BaseType::btFunct )
    {
	set_error(Program::DiagnosticPhase::runtime, "Program::execute() main is not a function");
	print_last_diagnostic(error());
	pop_runtime_scope();
	return;
    }
    if ( !(method=(Method *)var->data) )
    {
	set_error(Program::DiagnosticPhase::runtime, "Program::execute() main method is NULL");
	print_last_diagnostic(error());
	pop_runtime_scope();
	return;
    }
    if ( !(main_fn=(fVOIDFUNC)method->x86code) )
    {
	set_error(Program::DiagnosticPhase::runtime, "Program::execute() main has no x86 code");
	print_last_diagnostic(error());
	pop_runtime_scope();
	return;
    }
    DBG(std::cout << std::endl << "Program::execute() starts" << std::endl);
    DBG(std::cout << "Program::execute() calling main()[" << std::hex << ((uint64_t)main_fn) << std::dec << ']' << std::endl << std::endl);

    // check if main expects (int argc, char **argv)
    FuncDef *func = (FuncDef *)method->returns.type;
    if ( func->parameters.size() >= 2 )
    {
	typedef int (*fMAINARGS)(int64_t, char **);
	fMAINARGS main_args = (fMAINARGS)method->x86code;
	main_args((int64_t)script_argc, script_argv);
    }
    else
	main_fn();
    pop_runtime_scope();

    DBG(std::cout << std::endl << "Program::execute() main() returns" << std::endl);
    DBG(std::cout << "Program::execute() ends" << std::endl);
}

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







// LHS resolver shared by inc/dec and compound-assignment operators.
// Handles plain variables and struct members (via -> or .) and *ptr derefs.
// For member/deref, loads the Mem into a Gp and records the Mem for writeback.

// Load a sub-qword Mem into a full 64-bit Gp with proper sign/zero extension.
// Plain cc.mov(Gp64, Mem<2>) is not a legal x86 encoding — asmjit silently
// drops it or emits a 16-bit partial op, leaving the upper bits dirty and
// producing wrong arithmetic results.
void load_mem_to_gpq(Program &pgm, x86::Gp &gp, const x86::Mem &mem, DataDef *type)
{
    uint32_t sz = mem.size();
    if ( sz == 8 )       pgm.cc.mov(gp, mem);
    else if ( sz == 4 )
    {
	if ( type && type->is_unsigned() ) pgm.cc.mov(gp.r32(), mem); // implicit zero-extend
	else                               pgm.cc.movsxd(gp, mem);
    }
    else if ( sz == 2 )
    {
	if ( type && type->is_unsigned() ) pgm.cc.movzx(gp, mem);
	else                               pgm.cc.movsx(gp, mem);
    }
    else if ( sz == 1 )
    {
	if ( type && type->is_unsigned() ) pgm.cc.movzx(gp, mem);
	else                               pgm.cc.movsx(gp, mem);
    }
    else // fallback
	pgm.cc.mov(gp, mem);
}

void load_mem_to_xmm(Program &pgm, x86::Xmm &xmm, const x86::Mem &mem, DataDef *type)
{
    if ( type && type->is_simd() )
    {
	if ( type->size <= 8 )
	    pgm.cc.movq(xmm, mem);
	else
	    pgm.cc.movups(xmm, mem);
	return;
    }
    pgm.safemov(xmm, *(x86::Mem *)&mem, type, type);
}

void store_xmm_to_mem(Program &pgm, x86::Mem &mem, x86::Xmm &xmm, DataDef *type)
{
    if ( type && type->is_simd() )
    {
	if ( type->size <= 8 )
	    pgm.cc.movq(mem, xmm);
	else
	    pgm.cc.movups(mem, xmm);
	return;
    }
    pgm.safemov(mem, xmm, type, type);
}





// compile the increment operator.
// Supports plain variables (fast in-register inc) and struct members /
// *deref (load-inc-store). `left` = postfix (x++), `right` = prefix (++x).
// Shared shape for safeinc / safedec: in-place on the register/Mem.


















void emit_raw_aggregate_copy(Program &pgm, Operand &dst, Operand &src,
				    DataDef *copy_type, const char *name)
{
    if ( !copy_type )
	throw "emit_raw_aggregate_copy: missing type";
    if ( aggregate_has_runtime_size(copy_type) )
    {
	x86::Gp size_gp = emit_runtime_aggregate_size(pgm, copy_type, name);
	emit_runtime_aggregate_copy(pgm, dst, src, size_gp, name);
	return;
    }
    x86::Gp dst_gp = pgm.cc.newIntPtr("%s", name ? name : "aggregate_copy_dst");
    if ( dst.isMem() )
	pgm.cc.lea(dst_gp, dst.as<x86::Mem>());
    else if ( dst.isReg() && dst.as<BaseReg>().isGroup(RegGroup::kGp) )
	pgm.cc.mov(dst_gp, dst.as<x86::Gp>());
    else
	throw "emit_raw_aggregate_copy: unsupported destination operand";

    x86::Gp src_gp = pgm.cc.newIntPtr("%s", name ? name : "aggregate_copy_src");
    if ( src.isMem() )
	pgm.cc.lea(src_gp, src.as<x86::Mem>());
    else if ( src.isReg() && src.as<BaseReg>().isGroup(RegGroup::kGp) )
	pgm.cc.mov(src_gp, src.as<x86::Gp>());
    else
	throw "emit_raw_aggregate_copy: unsupported source operand";

    x86::Gp tmp = pgm.cc.newGpq("aggregate_copy_tmp");
    size_t copy_off = 0;
    size_t remaining = (size_t)copy_type->size;
    while ( remaining >= 8 )
    {
	x86::Mem src_mem = x86::ptr(src_gp, (int32_t)copy_off, 8);
	x86::Mem dst_mem = x86::ptr(dst_gp, (int32_t)copy_off, 8);
	pgm.cc.mov(tmp, src_mem);
	pgm.cc.mov(dst_mem, tmp.r64());
	copy_off += 8;
	remaining -= 8;
    }
    if ( remaining >= 4 )
    {
	x86::Mem src_mem = x86::ptr(src_gp, (int32_t)copy_off, 4);
	x86::Mem dst_mem = x86::ptr(dst_gp, (int32_t)copy_off, 4);
	pgm.cc.mov(tmp.r32(), src_mem);
	pgm.cc.mov(dst_mem, tmp.r32());
	copy_off += 4;
	remaining -= 4;
    }
    if ( remaining >= 2 )
    {
	x86::Mem src_mem = x86::ptr(src_gp, (int32_t)copy_off, 2);
	x86::Mem dst_mem = x86::ptr(dst_gp, (int32_t)copy_off, 2);
	pgm.cc.mov(tmp.r16(), src_mem);
	pgm.cc.mov(dst_mem, tmp.r16());
	copy_off += 2;
	remaining -= 2;
    }
    if ( remaining )
    {
	x86::Mem src_mem = x86::ptr(src_gp, (int32_t)copy_off, 1);
	x86::Mem dst_mem = x86::ptr(dst_gp, (int32_t)copy_off, 1);
	pgm.cc.mov(tmp.r8(), src_mem);
	pgm.cc.mov(dst_mem, tmp.r8());
    }
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
	_operand = newScalarXmm(pgm, _datatype, NULL);
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
    _const = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, _val);
    _operand = pgm.cc.newXmmSd("_real_const");
    DBG(pgm.cc.comment("TokenReal::operand() calling movsd(_operand.as<x86::Xmm>(), _const)"));
    DBG(cout << "TokenReal::operand() calling movsd(_operand.as<x86::Xmm>(), _const[" << _val << "])" << endl);
    pgm.cc.movsd(_operand.as<x86::Xmm>(), _const); //x86::qword_ptr((uintptr_t)&d_testval)); // x86::qword_ptr((uintptr_t)&_val));
    return _operand;
}

// variable needs special handling
Operand &TokenVar::operand(Program &pgm)
{
    Operand &op = pgm.tkFunction->voperand(pgm, &var);
    // Reference parameter: auto-dereference for assignment destinations
    if ( (var.flags & vfREFERENCE) && op.isReg()
      && op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	DataDef *base_type = var.type->is_pointer()
	    ? static_cast<DataDefPTR *>(var.type)->base_type : var.type;
	uint32_t sz = (uint32_t)base_type->size;
	if ( sz == 0 ) sz = 8;
	_operand = x86::ptr(op.as<x86::Gp>(), 0, sz);
	_datatype = base_type;
	return _operand;
    }
    return op;
}

// variable also needs to be able to write the register back to variable
void TokenVar::putreg(Program &pgm)
{
    pgm.tkFunction->putreg(pgm, &var);
}

static bool token_member_needs_runtime_offset(TokenMember *tm)
{
    if ( !tm )
	return false;
    DataDefSTRUCT *sdd = tm->owner_struct_type();
    if ( !sdd )
	return false;
    for ( size_t i = 0; i < sdd->members.size(); ++i )
    {
	if ( sdd->members[i].first == tm->var.name )
	    return false;
	if ( i < sdd->member_count_exprs.size() && sdd->member_count_exprs[i] )
	    return true;
    }
    return false;
}

static x86::Gp emit_token_member_runtime_offset(Program &pgm, TokenMember *tm,
						const char *name)
{
    x86::Gp offset_gp = pgm.cc.newGpq("%s", name ? name : "member_offset");
    pgm.cc.mov(offset_gp, imm((int64_t)(tm ? tm->offset : 0)));
    if ( !tm )
	return offset_gp;

    DataDefSTRUCT *sdd = tm->owner_struct_type();
    if ( !sdd )
	return offset_gp;
    for ( size_t i = 0; i < sdd->members.size(); ++i )
    {
	if ( sdd->members[i].first == tm->var.name )
	    break;
	TokenBase *count_expr = (i < sdd->member_count_exprs.size()) ? sdd->member_count_exprs[i] : NULL;
	if ( !count_expr )
	    continue;
	DataDef *member_type = sdd->members[i].second;
	size_t fixed_mult = (i < sdd->member_counts.size()) ? sdd->member_counts[i] : 1;
	size_t elem_bytes = member_type ? member_type->size * fixed_mult : 0;
	regdefp_t count_rdp = {NULL, NULL, NULL};
	Operand &count_op = count_expr->compile(pgm, count_rdp);
	x86::Gp count_gp = pgm.cc.newGpq("member_vla_count");
	load_idx_to_gpq(pgm, count_gp, count_op);
	if ( elem_bytes > 1 )
	    pgm.cc.imul(count_gp, count_gp, imm((int64_t)elem_bytes));
	pgm.cc.add(offset_gp, count_gp);
    }
    return offset_gp;
}

static Operand &emit_token_member_from_base_gp(Program &pgm, TokenMember *tm,
					       x86::Gp base_gp, Operand &out,
					       const char *addr_name)
{
    bool runtime_offset = token_member_needs_runtime_offset(tm);
    if ( tm->var.type->is_numeric() && !tm->is_fixed_array_member() )
    {
	if ( runtime_offset )
	{
	    x86::Gp off_gp = emit_token_member_runtime_offset(pgm, tm, "member_dyn_offset");
	    x86::Gp addr_gp = pgm.cc.newIntPtr("%s", addr_name ? addr_name : tm->var.name.c_str());
	    pgm.cc.lea(addr_gp, x86::ptr(base_gp, off_gp, 0, 0));
	    out = x86::ptr(addr_gp, 0, (uint32_t)tm->var.type->size);
	}
	else
	    out = x86::ptr(base_gp, (int32_t)tm->offset, (uint32_t)tm->var.type->size);
    }
    else
    {
	x86::Gp addr_gp = pgm.cc.newIntPtr("%s", addr_name ? addr_name : tm->var.name.c_str());
	if ( runtime_offset )
	{
	    x86::Gp off_gp = emit_token_member_runtime_offset(pgm, tm, "member_dyn_offset");
	    pgm.cc.lea(addr_gp, x86::ptr(base_gp, off_gp, 0, 0));
	}
	else
	    pgm.cc.lea(addr_gp, x86::ptr(base_gp, (int32_t)tm->offset));
	out = addr_gp;
    }
    return out;
}

Operand &TokenMember::operand(Program &pgm)
{
    if ( parent_expr != nullptr )
    {
	// Chained member access (e.g. ch->in_room->name or ch->desc.buf).
	// For a TokenMember parent, operand() re-materializes the parent's
	// address each call from its stored Variable/offset. For any other
	// expression parent (TokenCallFunc, TokenCallMethod, TokenSubscript,
	// TokenDerefExpr, ...), operand() returns an empty/fresh register
	// without emitting the computation, so we must compile() instead to
	// actually evaluate the pointer-producing expression.
	Operand *parent_op_ptr;
	regdefp_t parent_regdp;
	parent_regdp.first = NULL;
	parent_regdp.second = NULL;
	if ( parent_expr->type() == TokenType::ttMember )
	    parent_op_ptr = &parent_expr->operand(pgm);
	else
	    parent_op_ptr = &parent_expr->compile(pgm, parent_regdp);
	Operand &parent_op = *parent_op_ptr;
	bool parent_is_struct_value =
	    dynamic_cast<TokenSubscript *>(parent_expr) != NULL
	 || dynamic_cast<TokenSubscriptExpr *>(parent_expr) != NULL;

	if ( object.type->is_pointer() && !parent_is_struct_value )
	{
	    // Arrow chain: parent member holds a POINTER VALUE — load it into a Gp.
	    // e.g. ch->in_room->name: parent_op = Mem [ch + in_room_offset],
	    //      load that pointer, then compute [gp + name_offset].
	    x86::Gp obj_gp = pgm.cc.newIntPtr("%s", object.name.c_str());
	    DBG(pgm.cc.comment("TokenMember::operand() chained -> load intermediate ptr"));
	    if ( parent_op.isMem() )
		pgm.cc.mov(obj_gp, parent_op.as<x86::Mem>());
	    else
		pgm.cc.mov(obj_gp, parent_op.as<x86::Gp>());

	    if ( var.type->is_numeric() && !is_fixed_array_member() )
		_operand = x86::ptr(obj_gp, (int32_t)offset, (uint32_t)var.type->size);
	    else
	    {
		x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
		DBG(pgm.cc.comment("TokenMember::operand() chained -> lea non-numeric/array member"));
		pgm.cc.lea(addr_reg, x86::ptr(obj_gp, (int32_t)offset));
		_operand = addr_reg;
	    }
	}
	else
	{
	    // Dot chain: parent member is a STRUCT VALUE (not a pointer).
	    // The parent operand is either:
	    //   Gp  — parent LEA'd the address of the struct (typical: non-numeric structs)
	    //   Mem — parent returned a stack Mem (rare fallback)
	    if ( parent_op.isReg() )
	    {
		// e.g. ch->desc.buf: parent returned LEA of desc → use [gp + buf_offset]
		x86::Gp base_gp = parent_op.as<x86::Gp>();
		if ( var.type->is_numeric() && !is_fixed_array_member() )
		    _operand = x86::ptr(base_gp, (int32_t)offset, (uint32_t)var.type->size);
		else
		{
		    x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
		    DBG(pgm.cc.comment("TokenMember::operand() chained . lea from gp base"));
		    pgm.cc.lea(addr_reg, x86::ptr(base_gp, (int32_t)offset));
		    _operand = addr_reg;
		}
	    }
	    else
	    {
		// Mem-based fallback: addOffset to existing stack displacement
		x86::Mem member_mem = parent_op.as<x86::Mem>();
		member_mem.setSize(var.type->size);
		member_mem.addOffset((int64_t)offset);

		if ( var.type->is_numeric() && !is_fixed_array_member() )
		    _operand = member_mem;
		else
		{
		    x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
		    DBG(pgm.cc.comment("TokenMember::operand() chained . lea non-numeric/array member"));
		    pgm.cc.lea(addr_reg, member_mem);
		    _operand = addr_reg;
		}
	    }
	}
	return _operand;
    }

    Operand &_obj = pgm.tkFunction->voperand(pgm, &object); // make sure the parent object is all set up

    // Stack-backed POINTER variables (vfADDRTAKEN etc.): voperand returns
    // the Mem slot holding the pointer *value*. Load it into a Gp first —
    // otherwise the addOffset below would walk into the pointer's storage
    // instead of the struct the pointer points at.
    if ( _obj.isMem() && object.type->is_pointer() )
    {
	x86::Gp obj_gp = pgm.cc.newIntPtr("%s", object.name.c_str());
	DBG(pgm.cc.comment("TokenMember::operand() load stack-backed pointer into Gp"));
	pgm.cc.mov(obj_gp, _obj.as<x86::Mem>());
	DBG(pgm.cc.comment("TokenMember::operand() stack-backed pointer member"));
	emit_token_member_from_base_gp(pgm, this, obj_gp, _operand, var.name.c_str());
	return _operand;
    }

    if ( _obj.isMem() )
    {
	// Struct/array on the JIT stack: compute [struct_base + member_offset]
	if ( token_member_needs_runtime_offset(this) )
	{
	    x86::Gp obj_gp = pgm.cc.newIntPtr("%s", object.name.c_str());
	    DBG(pgm.cc.comment("TokenMember::operand() runtime member offset from stack base"));
	    pgm.cc.lea(obj_gp, _obj.as<x86::Mem>());
	    emit_token_member_from_base_gp(pgm, this, obj_gp, _operand, var.name.c_str());
	    return _operand;
	}
	x86::Mem member_mem = _obj.as<x86::Mem>();
	member_mem.setSize(var.type->size);
	member_mem.addOffset((int64_t)offset);

	if ( var.type->is_numeric() && !is_fixed_array_member() )
	{
	    // For numeric members: return the Mem so assignments go directly to memory
	    _operand = member_mem;
	}
	else
	{
	    // For string/object members: return address (pointer) in a Gp register
	    // so functions like string_assign can receive the destination pointer
	    x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
	    DBG(pgm.cc.comment("TokenMember::operand() lea addr of non-numeric member"));
	    pgm.cc.lea(addr_reg, member_mem);
	    _operand = addr_reg;
	}
    }
    else if ( _obj.isReg() && _obj.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	// Object is a pointer in a Gp register (e.g. __this in class methods, or -> access)
	// Access member at [gp + offset]
	x86::Gp obj_gp = _obj.as<x86::Gp>();
	DBG(pgm.cc.comment("TokenMember::operand() member from pointer base"));
	emit_token_member_from_base_gp(pgm, this, obj_gp, _operand, var.name.c_str());
    }
    else
    {
	// Fallback
	_operand = _obj.clone();
    }
    return _operand;
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
    pgm.tkFunction->putreg(pgm, &var);
}

Operand &TokenCallFunc::operand(Program &pgm)
{
    _operand = returns()->newreg(pgm.cc, var.name.c_str());
    DBG(pgm.cc.comment("TokenCallFunc::operand"));
    DBG(cout << "TokenCallFunc::operand() size " << _operand.x86RmSize() << endl);
    return _operand;
}

Operand &TokenCallMethod::operand(Program &pgm)
{
    _operand = returns()->newreg(pgm.cc, var.name.c_str());
    return _operand;
}

void bind_call_return(Program &pgm, InvokeNode *call, Operand *dest, DataDef *ret_type,
			     Operand &fallback_operand, bool is_variadic, bool narrow_int_ret)
{
    if ( !ret_type || ret_type->type() == DataType::dtVOID )
	return;
    bool ret_in_xmm = ret_type->is_real() || ret_type->is_simd();

    // narrow_int_ret: libc dlsym functions that return a 32-bit int (strcmp,
    // memcmp, printf, fcntl, ...) place the result in EAX; the upper 32 bits
    // of RAX are indeterminate, so madc — which treats returns as int64 —
    // sign-extends via movsxd. Only meaningful on non-real returns.
    if ( narrow_int_ret && !ret_type->is_real() )
    {
	x86::Gp ret_gp = pgm.cc.newGpq("dl_ret");
	call->setRet(0, ret_gp);
	pgm.cc.movsxd(ret_gp, ret_gp.r32());
	if ( !dest )
	{
	    fallback_operand = ret_gp;
	    return;
	}
	if ( dest->isReg() && dest->as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    // asmjit's intermediate validator rejects `mov gpw, gpq`
	    // even when the destination is a narrower-allocated vreg.
	    // Move via the 64-bit view; the dest vreg's natural width
	    // already truncates downstream consumers correctly.
	    pgm.cc.mov(dest->as<x86::Gp>().r64(), ret_gp);
	}
	else if ( dest->isMem() )
	{
	    IRBuilder ir(pgm.cc);
	    ir.store(IRValue::mem(dest->as<x86::Mem>(), ret_type), ir_from_operand(ret_gp, ret_type));
	}
	else
	    throw "bind_call_return() narrow_int_ret: unsupported destination";
	return;
    }

    if ( !dest )
    {
	if ( !fallback_operand.isReg()
	  || (ret_in_xmm && !fallback_operand.as<BaseReg>().isGroup(RegGroup::kVec))
	  || (!ret_in_xmm && !fallback_operand.as<BaseReg>().isGroup(RegGroup::kGp)) )
	    fallback_operand = ret_type->newreg(pgm.cc, "call_ret");
	if ( ret_in_xmm )
	    call->setRet(0, fallback_operand.as<x86::Xmm>());
	else
	    call->setRet(0, fallback_operand.as<x86::Gp>());
	return;
    }

    if ( dest->isReg() )
    {
	if ( dest->as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    if ( is_variadic && ret_type->is_real() )
	    {
		x86::Xmm ret_xmm = newScalarXmm(pgm, ret_type, "dl_ret");
		call->setRet(0, ret_xmm);
		pgm.cc.movsd(dest->as<x86::Xmm>(), ret_xmm);
	    }
	    else
		call->setRet(0, dest->as<x86::Xmm>());
	    return;
	}
	if ( dest->as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    call->setRet(0, dest->as<x86::Gp>());
	    return;
	}
	throw "bind_call_return() unsupported register destination";
    }

    if ( dest->isMem() )
    {
	if ( ret_in_xmm )
	{
	    x86::Xmm ret_xmm = ret_type->newreg(pgm.cc, "call_ret").as<x86::Xmm>();
	    call->setRet(0, ret_xmm);
	    if ( ret_type->is_simd() )
	    {
		x86::Mem mem = dest->as<x86::Mem>();
		store_xmm_to_mem(pgm, mem, ret_xmm, ret_type);
	    }
	    else
	    {
		IRBuilder ir(pgm.cc);
		ir.store(IRValue::mem(dest->as<x86::Mem>(), ret_type), ir_from_operand(ret_xmm, ret_type));
	    }
	}
	else
	{
	    Operand tmp = ret_type->newreg(pgm.cc, "call_ret");
	    if ( !tmp.isReg() || !tmp.as<BaseReg>().isGroup(RegGroup::kGp) )
		throw "bind_call_return() non-real return did not allocate gp register";
	    call->setRet(0, tmp.as<x86::Gp>());
	    IRBuilder ir(pgm.cc);
	    ir.store(IRValue::mem(dest->as<x86::Mem>(), ret_type), ir_from_operand(tmp.as<x86::Gp>(), ret_type));
	}
	return;
    }

    throw "bind_call_return() unsupported destination operand";
}

#if 0
// function needs similar handling to variable
x86::Gp &TokenCallFunc::getreg(Program &pgm)
{
    _operand = returns()->newreg(pgm.cc, var.name.c_str());
    return _operand.as<x86::Gp>();
}
#endif

void TokenCpnd::movreg(Program &pgm, Operand &op, Variable *var)
{
    if ( !op.isReg() )
    {
	DBG(pgm.cc.comment("TokenCpnd::movreg() operand is not a register"));
	return;
    }
    if ( pgm.aot_tracking && var && (var->data || var->has_aot_data()) )
    {
	x86::Gp base = pgm.cc.newIntPtr("%s_aot_base", var->name.c_str());
	pgm.emit_data_mov(base, var);
	x86::Mem src = x86::ptr(base, 0, (uint32_t)var->type->size);
	if ( op.as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    pgm.safemov(op.as<x86::Xmm>(), src, var->type, var->type);
	    return;
	}
	if ( op.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    // For aggregate-like globals (std::string, streams, arrays,
	    // structs/classes), the Gp operand convention is "address of the
	    // object", not "load the first machine word from the object".
	    // AOT mode must honor that for every addressable global, not just
	    // ones routed through has_aot_data(); literal-map strings and
	    // other raw-data globals still live in the exported .data image.
	    if ( !var->type->is_numeric() && !var->type->is_pointer() )
		pgm.cc.mov(op.as<x86::Gp>(), base);
	    else
		pgm.safemov(op.as<x86::Gp>(), src, var->type, var->type);
	    return;
	}
    }
    if ( op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	DBG(pgm.cc.comment("TokenCpnd::movreg() calling movmptr2xval(cc, reg, var->data)"));
	DBG(pgm.cc.comment(var->name.c_str()));
	var->type->movmptr2xval(pgm.cc, op.as<x86::Xmm>(), var->data);
    }
    else if ( op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	DBG(pgm.cc.comment("TokenCpnd::movreg() calling movmptr2rval(cc, reg, var->data)"));
	DBG(pgm.cc.comment(var->name.c_str()));
	var->type->movmptr2rval(pgm.cc, op.as<x86::Gp>(), var->data);
    }
    else
    {
	throw "TokenCpnd::movreg() unsupported operand";
    }
}

// Read subscript: container[index]
Operand &TokenSubscript::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenSubscript::compile()"));
    IRBuilder ir(pgm.cc);

    // get container pointer. VLAs / address-taken pointer locals stash
    // the pointer in a stack slot — load the value before indexing.
    Operand &obj_op = pgm.tkFunction->voperand(pgm, &object);
    x86::Gp obj_reg = pgm.cc.newIntPtr("sub_obj");
    if ( obj_op.isMem() )
    {
	if ( subscript_object_uses_inplace_storage(object) )
	    pgm.cc.lea(obj_reg, obj_op.as<x86::Mem>());
	else
	    pgm.cc.mov(obj_reg, obj_op.as<x86::Mem>());
    }
    else
	pgm.cc.mov(obj_reg, obj_op.as<x86::Gp>());

    // compile index expression
    regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
    Operand &idx_op = index->compile(pgm, idx_rdp);
    x86::Gp idx_reg = pgm.cc.newGpq("sub_idx");
    load_idx_to_gpq(pgm, idx_reg, idx_op);

    DataType ctype = object.type->type();

    // C fixed-size array: load element directly from [base + linear_idx*elem_size]
    if ( object.is_fixed_array() )
    {
        size_t consumed_dims = 1 + extra_indices.size();
        DataDef *result_type = fixed_array_subscript_result_type(object, consumed_dims);
        size_t elem_size = fixed_array_subscript_stride(object, consumed_dims);
        // Fold any extra indices (multi-dim): linear = ((i0*d1) + i1)*d2 + i2 ...
        for ( size_t k = 0; k < extra_indices.size(); ++k )
        {
            uint32_t dim_k = object.dims[k + 1];
            pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)dim_k));
            regdefp_t ex_rdp = {nullptr, nullptr, nullptr};
            Operand &ex_op = extra_indices[k]->compile(pgm, ex_rdp);
            if ( ex_op.isImm() )
                pgm.cc.add(idx_reg, ex_op.as<Imm>());
            else
            {
                x86::Gp ex_widened = pgm.cc.newGpq("ex_idx_widened");
                load_idx_to_gpq(pgm, ex_widened, ex_op);
                pgm.cc.add(idx_reg, ex_widened);
            }
        }

        // Struct-element array: return a Gp pointer to the element (no load).
        // Callers (TokenMember for dot access) treat it as the struct's base.
        if ( dynamic_cast<DataDefSTRUCT *>(object.type) != NULL
          && object.type->type() == DataType::dtRESERVED )
        {
            DBG(pgm.cc.comment("fixed-array subscript: struct element → pointer"));
            x86::Gp addr = pgm.cc.newIntPtr("sub_addr");
            pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)elem_size));
            pgm.cc.lea(addr, x86::ptr(obj_reg, idx_reg, 0, 0));
            _operand = addr;
            if ( !regdp.second )
                regdp.second = _datatype;
            regdp.first = &_operand;
            return _operand;
        }

        uint32_t shift = scale_index_by_element_size(pgm, idx_reg, result_type, "fixed_sub_size");
        DBG(pgm.cc.comment("fixed-array subscript read"));
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
        if ( !regdp.second )
            regdp.second = result_type;
        if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(result_type) )
        {
            DataDef *elem_base = add->element_type ? add->element_type : &ddINT64;
            DataDef *decay_type = pgm.getPointerType(elem_base);
            regdp.second = decay_type;
            x86::Gp addr = as_gp_ptr(pgm, elem_mem, "sub_arr");
            return emit_ir_value(pgm, IRValue::reg(addr, decay_type), regdp, _operand, decay_type);
        }
        return emit_ir_value(pgm, IRValue::mem(elem_mem, result_type), regdp, _operand, result_type);
    }

    // Raw pointer subscript: ptr[i] == *(ptr + i).
    // obj_reg already holds the pointer value (from voperand). Compute
    // [ptr + idx * sizeof(base_type)] with SIB scale when possible.
    if ( !object.is_fixed_array() && object.type->is_pointer() )
    {
        DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(object.type);
        DataDef *base = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
        uint32_t shift = scale_index_by_element_size(pgm, idx_reg, base, "ptr_sub_size");
        DBG(pgm.cc.comment("pointer subscript read"));
        size_t elem_size = base && base->size ? base->size : 8;
        if ( aggregate_has_runtime_size(base)
          && (base->basetype() == BaseType::btStruct || base->basetype() == BaseType::btClass) )
        {
            x86::Gp addr = pgm.cc.newIntPtr("sub_addr");
            pgm.cc.lea(addr, x86::ptr(obj_reg, idx_reg, shift, 0));
            _operand = addr;
            if ( !regdp.second )
                regdp.second = _datatype;
            regdp.first = &_operand;
            return _operand;
        }
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
        if ( base && (base->basetype() == BaseType::btStruct || base->basetype() == BaseType::btClass) )
        {
            x86::Gp addr = pgm.cc.newIntPtr("sub_addr");
            pgm.cc.lea(addr, x86::ptr(obj_reg, idx_reg, shift, 0));
            _operand = addr;
            if ( !regdp.second )
                regdp.second = _datatype;
            regdp.first = &_operand;
            return _operand;
        }
        if ( !regdp.second )
            regdp.second = _datatype;
        if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(_datatype) )
        {
            DataDef *elem_base = add->element_type ? add->element_type : &ddINT64;
            DataDef *decay_type = pgm.getPointerType(elem_base);
            regdp.second = decay_type;
            x86::Gp addr = as_gp_ptr(pgm, elem_mem, "sub_arr");
            return emit_ir_value(pgm, IRValue::reg(addr, decay_type), regdp, _operand, decay_type);
        }
        return emit_ir_value(pgm, IRValue::mem(elem_mem, _datatype), regdp, _operand, _datatype);
    }

    if ( ctype == DataType::dtSIMD )
    {
	DataDefSIMD *vdd = static_cast<DataDefSIMD *>(object.type);
	DataDef *elem_type = (vdd && vdd->element_type) ? vdd->element_type : &ddINT64;
	size_t elem_size = elem_type->size ? elem_type->size : 8;
	uint32_t shift = scale_index_by_element_size(pgm, idx_reg, elem_type, "simd_sub_size");
	x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
	if ( !regdp.second )
	    regdp.second = elem_type;
	return emit_ir_value(pgm, IRValue::mem(elem_mem, elem_type), regdp, _operand, elem_type);
    }

    if ( ctype == DataType::dtSTRING )
    {
	InvokeNode *call;
	pgm.cc.invoke(&call, imm(string_cstr), FuncSignature::build<const char *, void *>());
	call->setArg(0, obj_reg);
	x86::Gp cstr = pgm.cc.newIntPtr("sub_str_cstr");
	call->setRet(0, cstr);
	x86::Mem elem_mem = x86::ptr(cstr, idx_reg, 0, 0, 1);
	if ( !regdp.second )
	    regdp.second = _datatype;
	return emit_ir_value(pgm, IRValue::mem(elem_mem, _datatype), regdp, _operand, _datatype);
    }

    if ( ctype == DataType::dtVECTOR )
    {
        DataDefVECTOR *vdd = static_cast<DataDefVECTOR *>(object.type);
        if ( vdd->element_type->is_string() )
        {
            // vector<string>[i] → vector_str_at(result, ptr, i)
            Operand &tmp_op = pgm.tkFunction->voperand(pgm, tmp_var);
            InvokeNode *call;
            DBG(pgm.cc.comment("vector_str_at"));
            pgm.cc.invoke(&call, imm(vector_str_at),
                FuncSignature::build<void *, void *, void *, int64_t>());
            call->setArg(0, tmp_op.as<x86::Gp>());
            call->setArg(1, obj_reg);
            call->setArg(2, idx_reg);
            _operand = tmp_op;
        }
        else
        {
            // vector<int>[i] → vector_int_at(ptr, i)
            x86::Gp res = pgm.cc.newGpq("sub_res");
            InvokeNode *call;
            DBG(pgm.cc.comment("vector_int_at"));
            pgm.cc.invoke(&call, imm(vector_int_at),
                FuncSignature::build<int64_t, void *, int64_t>());
            call->setArg(0, obj_reg);
            call->setArg(1, idx_reg);
            call->setRet(0, res);
            _operand = res;
        }
    }
    else if ( ctype == DataType::dtMAP )
    {
        DataDefMAP *mdd = static_cast<DataDefMAP *>(object.type);
        if ( mdd->val_type->is_string() )
        {
            // map<string,string>["k"] → map_str_str_get(result, ptr, key)
            Operand &tmp_op = pgm.tkFunction->voperand(pgm, tmp_var);
            InvokeNode *call;
            DBG(pgm.cc.comment("map_str_str_get"));
            pgm.cc.invoke(&call, imm(map_str_str_get),
                FuncSignature::build<void *, void *, void *, void *>());
            call->setArg(0, tmp_op.as<x86::Gp>());
            call->setArg(1, obj_reg);
            call->setArg(2, idx_reg);
            _operand = tmp_op;
        }
        else
        {
            // map<string,int>["k"] → map_str_int_get(ptr, key)
            x86::Gp res = pgm.cc.newGpq("sub_res");
            InvokeNode *call;
            DBG(pgm.cc.comment("map_str_int_get"));
            pgm.cc.invoke(&call, imm(map_str_int_get),
                FuncSignature::build<int64_t, void *, void *>());
            call->setArg(0, obj_reg);
            call->setArg(1, idx_reg);
            call->setRet(0, res);
            _operand = res;
        }
    }
    else // dtARRAY (MadArray) — int-indexed read
    {
        // array[i] → php_array_get_int(ptr, i)
        x86::Gp res = pgm.cc.newGpq("sub_res");
        InvokeNode *call;
        DBG(pgm.cc.comment("php_array_get_int"));
        pgm.cc.invoke(&call, imm(php_array_get_int),
            FuncSignature::build<int64_t, void *, int64_t>());
        call->setArg(0, obj_reg);
        call->setArg(1, idx_reg);
        call->setRet(0, res);
        _operand = res;
    }

    if ( !regdp.second )
        regdp.second = _datatype;

    // Numeric / pointer container reads normalize through the IR so a
    // caller's regdp.first=Mem destination is honored (writing the
    // result into the caller's Mem via store) instead of being silently
    // overwritten by our local `_operand` Gp.
    if ( _datatype && (_datatype->is_numeric() || _datatype->is_pointer())
      && _operand.isReg() )
        return emit_ir_value(pgm, IRValue::reg(_operand, _datatype), regdp, _operand, _datatype);

    regdp.first = &_operand;
    return _operand;
}

Operand &TokenSubscriptExpr::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenSubscriptExpr::compile()"));

    // operand() keeps the raw Mem/Gp for aggregate bases; compile() would
    // load-into-Gp via emit_ir_value for numeric-typed TokenMember bases
    // (e.g. struct-contained `int bits[4]`) and destroy the array address.
    // Exception: complex value-producing operators (TokenAdd/Sub/Assign for
    // `(p + n)[i]`, `(q = p + 1)[i]`) DON'T have a meaningful operand() —
    // they need compile() to actually emit the arithmetic and yield a Reg
    // holding the pointer value.
    bool base_needs_compile =
	dynamic_cast<TokenAdd *>(base_expr) != NULL
     || dynamic_cast<TokenSub *>(base_expr) != NULL
     || dynamic_cast<TokenAssign *>(base_expr) != NULL
     || dynamic_cast<TokenCast *>(base_expr) != NULL
     || dynamic_cast<TokenComma *>(base_expr) != NULL;
    Operand op_storage;
    Operand *op_ptr;
    if ( base_needs_compile )
    {
	regdefp_t base_rdp = {nullptr, base_expr->datadef(), nullptr};
	op_ptr = &base_expr->compile(pgm, base_rdp);
    }
    else
	op_ptr = &base_expr->operand(pgm);
    Operand &base_op = *op_ptr;
    x86::Gp base_reg = pgm.cc.newIntPtr("subexpr_obj");
    if ( base_op.isMem() )
    {
	// If base_expr is a pointer-typed value (e.g. `s->items` where items
	// is `int *`), the Mem holds the pointer value — dereference it with
	// mov to get the real array address. If base_expr is an aggregate
	// stored in-place (fixed-array variable, nested struct), we want the
	// address OF the Mem, which LEA gives. Pick based on datadef.
	DataDef *bdd = base_expr->datadef();
	if ( bdd && bdd->is_pointer() && !is_fixed_array_struct_member(base_expr) )
	    pgm.cc.mov(base_reg, base_op.as<x86::Mem>());
	else
	    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
    }
    else if ( base_op.isReg() && base_op.as<BaseReg>().isGroup(RegGroup::kVec)
	   && base_expr->datadef() && base_expr->datadef()->is_simd() )
    {
	x86::Mem slot = pgm.cc.newStack((uint32_t)base_expr->datadef()->size, 16);
	store_xmm_to_mem(pgm, slot, base_op.as<x86::Xmm>(), base_expr->datadef());
	pgm.cc.lea(base_reg, slot);
    }
    else if ( base_op.isReg() && base_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	pgm.cc.mov(base_reg, base_op.as<x86::Gp>());
    else
	pgm.Throw(this) << "TokenSubscriptExpr::compile() unsupported base operand" << flush;

    regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
    Operand &idx_op = index->compile(pgm, idx_rdp);
    x86::Gp idx_reg = pgm.cc.newGpq("subexpr_idx");
    load_idx_to_gpq(pgm, idx_reg, idx_op);

    DataDef *result_type = _datatype;
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(base_expr) )
	if ( tm->is_fixed_array_member() )
	    result_type = fixed_array_member_result_type(tm, 1);

    size_t elem_size = result_type && result_type->size ? result_type->size : 8;
    uint32_t shift = scale_index_by_element_size(pgm, idx_reg, result_type, "subexpr_size");

    x86::Mem elem_mem = x86::ptr(base_reg, idx_reg, shift, 0, (uint32_t)elem_size);
    if ( !regdp.second )
	regdp.second = result_type;
    // C array-to-pointer decay in value context: `arr2[i]` where the
    // element itself is an array (e.g. `unsigned long x[2][2];
    // x[i]`) yields a pointer to the first element, not the first
    // scalar value loaded from that subarray. Keep operand() as the raw
    // lvalue Mem for address-taking/chaining, but compile() must decay.
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(result_type) )
    {
	DataDef *elem_base = add->element_type ? add->element_type : &ddINT64;
	DataDef *decay_type = pgm.getPointerType(elem_base);
	regdp.second = decay_type;
	x86::Gp addr = as_gp_ptr(pgm, elem_mem, "subexpr_arr");
	return emit_ir_value(pgm, IRValue::reg(addr, decay_type), regdp, _operand, decay_type);
    }
    // Struct/class element: return the Mem directly. emit_ir_value assumes
    // a numeric-sized value it can coerce/load; a struct element isn't a
    // value that fits in a register. Callers (e.g. TokenMember::operand
    // for `s[i].member`) read the Mem and add the member offset.
    if ( result_type && (result_type->basetype() == BaseType::btStruct
		      || result_type->basetype() == BaseType::btClass) )
    {
	_operand = elem_mem;
	if ( !regdp.first )
	    regdp.first = &_operand;
	return _operand;
    }
    return emit_ir_value(pgm, IRValue::mem(elem_mem, result_type), regdp, _operand, result_type);
}

DataDef *TokenSubscript::datadef() const
{
    if ( object.is_fixed_array() )
	return fixed_array_subscript_result_type(object, 1 + extra_indices.size());
    return _datatype;
}

// Lvalue path — return the Mem pointing at the element address. Mirrors
// the address-computing portion of compile() but stops before
// emit_ir_value so callers can continue chaining (e.g. nested 2D
// subscripts, TokenMember on s[i], `&arr[i]`).
Operand &TokenSubscriptExpr::operand(Program &pgm)
{
    DBG(pgm.cc.comment("TokenSubscriptExpr::operand()"));

    bool base_needs_compile =
	dynamic_cast<TokenAdd *>(base_expr) != NULL
     || dynamic_cast<TokenSub *>(base_expr) != NULL
     || dynamic_cast<TokenAssign *>(base_expr) != NULL
     || dynamic_cast<TokenCast *>(base_expr) != NULL
     || dynamic_cast<TokenComma *>(base_expr) != NULL;
    Operand op_storage;
    Operand *op_ptr;
    if ( base_needs_compile )
    {
	regdefp_t base_rdp = {nullptr, base_expr->datadef(), nullptr};
	op_ptr = &base_expr->compile(pgm, base_rdp);
    }
    else
	op_ptr = &base_expr->operand(pgm);
    Operand &base_op = *op_ptr;
    x86::Gp base_reg = pgm.cc.newIntPtr("subexpr_obj");
    if ( base_op.isMem() )
    {
	DataDef *bdd = base_expr->datadef();
	if ( bdd && bdd->is_pointer() && !is_fixed_array_struct_member(base_expr) )
	    pgm.cc.mov(base_reg, base_op.as<x86::Mem>());
	else
	    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
    }
    else if ( base_op.isReg() && base_op.as<BaseReg>().isGroup(RegGroup::kVec)
	   && base_expr->datadef() && base_expr->datadef()->is_simd() )
    {
	x86::Mem slot = pgm.cc.newStack((uint32_t)base_expr->datadef()->size, 16);
	store_xmm_to_mem(pgm, slot, base_op.as<x86::Xmm>(), base_expr->datadef());
	pgm.cc.lea(base_reg, slot);
    }
    else if ( base_op.isReg() && base_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	pgm.cc.mov(base_reg, base_op.as<x86::Gp>());
    else
	pgm.Throw(this) << "TokenSubscriptExpr::operand() unsupported base operand" << flush;

    regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
    Operand &idx_op = index->compile(pgm, idx_rdp);
    x86::Gp idx_reg = pgm.cc.newGpq("subexpr_idx");
    load_idx_to_gpq(pgm, idx_reg, idx_op);

    DataDef *result_type = _datatype;
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(base_expr) )
	if ( tm->is_fixed_array_member() )
	    result_type = fixed_array_member_result_type(tm, 1);

    size_t elem_size = result_type && result_type->size ? result_type->size : 8;
    DBG(pgm.cc.comment(("subexpr_operand elem_size=" + std::to_string(elem_size)).c_str()));
    uint32_t shift = scale_index_by_element_size(pgm, idx_reg, result_type, "subexpr_size");

    _operand = x86::ptr(base_reg, idx_reg, shift, 0, (uint32_t)elem_size);
    return _operand;
}

// Write subscript: container[index] = val  (called from TokenAssign::compile)
void TokenSubscript::compile_set(Program &pgm, Operand &val_op, DataDef *val_type)
{
    DBG(pgm.cc.comment("TokenSubscript::compile_set()"));
    IRBuilder ir(pgm.cc);
    auto store_complex_value = [&](x86::Mem dst_mem, DataDef *dst_type) -> bool {
	DataDefCOMPLEX *dst_cdd = dynamic_cast<DataDefCOMPLEX *>(dst_type);
	if ( !dst_cdd || !dst_cdd->element_type )
	    return false;

	x86::Mem dst_real = complex_component_mem(pgm, dst_mem, dst_cdd, false, "sub_complex_dst");
	x86::Mem dst_imag = complex_component_mem(pgm, dst_mem, dst_cdd, true, "sub_complex_dst");

	if ( val_type && val_type->is_complex() )
	{
	    DataDefCOMPLEX *src_cdd = dynamic_cast<DataDefCOMPLEX *>(val_type);
	    if ( !src_cdd || !src_cdd->element_type )
		throw "TokenSubscript::compile_set() invalid complex source type";
	    x86::Mem src_real = complex_component_mem(pgm, val_op, src_cdd, false, "sub_complex_src");
	    x86::Mem src_imag = complex_component_mem(pgm, val_op, src_cdd, true, "sub_complex_src");
	    IRValue real_val = ir.load(ir.coerce(IRValue::mem(src_real, src_cdd->element_type),
						 dst_cdd->element_type));
	    IRValue imag_val = ir.load(ir.coerce(IRValue::mem(src_imag, src_cdd->element_type),
						 dst_cdd->element_type));
	    ir.store(IRValue::mem(dst_real, dst_cdd->element_type), real_val);
	    ir.store(IRValue::mem(dst_imag, dst_cdd->element_type), imag_val);
	    return true;
	}

	IRValue scalar_val = ir.load(ir.coerce(ir_from_operand(val_op, val_type ? val_type : dst_cdd->element_type),
						dst_cdd->element_type));
	ir.store(IRValue::mem(dst_real, dst_cdd->element_type), scalar_val);
	ir.store(IRValue::mem(dst_imag, dst_cdd->element_type), IRValue::imm(Imm(0), &ddINT));
	return true;
    };

    // get container pointer. VLAs and address-taken pointer locals live
    // in a stack-resident slot — load the pointer value before indexing.
    Operand &obj_op = pgm.tkFunction->voperand(pgm, &object);
    x86::Gp obj_reg = pgm.cc.newIntPtr("sub_obj");
    if ( obj_op.isMem() )
    {
	if ( subscript_object_uses_inplace_storage(object) )
	    pgm.cc.lea(obj_reg, obj_op.as<x86::Mem>());
	else
	    pgm.cc.mov(obj_reg, obj_op.as<x86::Mem>());
    }
    else
	pgm.cc.mov(obj_reg, obj_op.as<x86::Gp>());

    // compile index expression
    regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
    Operand &idx_op = index->compile(pgm, idx_rdp);
    x86::Gp idx_reg = pgm.cc.newGpq("sub_idx");
    load_idx_to_gpq(pgm, idx_reg, idx_op);

    DataType ctype = object.type->type();

    // C fixed-size array: store element directly to [base + linear_idx*elem_size]
    if ( object.is_fixed_array() )
    {
        size_t consumed_dims = 1 + extra_indices.size();
        DataDef *result_type = fixed_array_subscript_result_type(object, consumed_dims);
        size_t elem_size = fixed_array_subscript_stride(object, consumed_dims);
        DBG(pgm.cc.comment("fixed-array subscript write"));
        // Fold extra indices into idx_reg using dims: linear = ((i0*d1)+i1)*d2 + i2 ...
        for ( size_t k = 0; k < extra_indices.size(); ++k )
        {
            uint32_t dim_k = object.dims[k + 1];
            pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)dim_k));
            regdefp_t ex_rdp = {nullptr, nullptr, nullptr};
            Operand &ex_op = extra_indices[k]->compile(pgm, ex_rdp);
            if ( ex_op.isImm() )
                pgm.cc.add(idx_reg, ex_op.as<Imm>());
            else
            {
                x86::Gp ex_widened = pgm.cc.newGpq("ex_idx_widened");
                load_idx_to_gpq(pgm, ex_widened, ex_op);
                pgm.cc.add(idx_reg, ex_widened);
            }
        }
        uint32_t shift = scale_index_by_element_size(pgm, idx_reg, result_type, "fixed_sub_size");
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
	if ( _datatype && _datatype->is_complex() )
	{
	    store_complex_value(elem_mem, _datatype);
	    return;
	}
	if ( _datatype && (_datatype->basetype() == BaseType::btStruct
			|| _datatype->basetype() == BaseType::btClass) )
	{
	    Operand dst_slot = elem_mem;
	    emit_raw_aggregate_copy(pgm, dst_slot, val_op,
		_datatype, "fixed_sub_struct_copy");
	    return;
	}
        ir.store(IRValue::mem(elem_mem, _datatype), ir_from_operand(val_op, val_type ? val_type : _datatype));
        return;
    }

    // Raw pointer subscript write: ptr[i] = val  →  *(ptr + i*elem) = val
    if ( !object.is_fixed_array() && object.type->is_pointer() )
    {
        DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(object.type);
        DataDef *base = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
        size_t elem_size = base->size ? base->size : 8;
        uint32_t shift = scale_index_by_element_size(pgm, idx_reg, base, "ptr_sub_size");
        DBG(pgm.cc.comment("pointer subscript write"));
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
        if ( base && base->is_complex() )
        {
	    store_complex_value(elem_mem, base);
	    return;
        }
        if ( base && (base->basetype() == BaseType::btStruct || base->basetype() == BaseType::btClass) )
        {
	    Operand dst_slot = elem_mem;
	    emit_raw_aggregate_copy(pgm, dst_slot, val_op, base, "ptr_sub_struct_copy");
	    return;
        }
        ir.store(IRValue::mem(elem_mem, base), ir_from_operand(val_op, val_type ? val_type : base));
        return;
    }

    // SIMD vector subscript write: v[i] = val → byte/word/dword store
    // at [base + i * elem_size].  Both Mem-backed (>16 bytes) and
    // register-backed SIMD variables need the address via LEA.
    if ( ctype == DataType::dtSIMD )
    {
	DataDefSIMD *vdd = static_cast<DataDefSIMD *>(object.type);
	DataDef *elem = vdd->element_type ? vdd->element_type : &ddINT;
	size_t elem_size = elem->size ? elem->size : 1;
	uint32_t shift = scale_index_by_element_size(pgm, idx_reg, elem, "simd_sub_size");
	x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
	ir.store(IRValue::mem(elem_mem, elem), ir_from_operand(val_op, val_type ? val_type : elem));
	return;
    }

    if ( ctype == DataType::dtVECTOR )
    {
        DataDefVECTOR *vdd = static_cast<DataDefVECTOR *>(object.type);
        if ( vdd->element_type->is_string() )
        {
            InvokeNode *call;
            DBG(pgm.cc.comment("vector_str_set"));
            pgm.cc.invoke(&call, imm(vector_str_set),
                FuncSignature::build<void, void *, int64_t, void *>());
            call->setArg(0, obj_reg);
            call->setArg(1, idx_reg);
            call->setArg(2, val_op.as<x86::Gp>());
        }
        else
        {
            InvokeNode *call;
            DBG(pgm.cc.comment("vector_int_set"));
            pgm.cc.invoke(&call, imm(vector_int_set),
                FuncSignature::build<void, void *, int64_t, int64_t>());
            call->setArg(0, obj_reg);
            call->setArg(1, idx_reg);
            call->setArg(2, val_op.as<x86::Gp>());
        }
    }
    else if ( ctype == DataType::dtMAP )
    {
        DataDefMAP *mdd = static_cast<DataDefMAP *>(object.type);
        if ( mdd->val_type->is_string() )
        {
            InvokeNode *call;
            DBG(pgm.cc.comment("map_str_str_set"));
            pgm.cc.invoke(&call, imm(map_str_str_set),
                FuncSignature::build<void, void *, void *, void *>());
            call->setArg(0, obj_reg);
            call->setArg(1, idx_reg);
            call->setArg(2, val_op.as<x86::Gp>());
        }
        else
        {
            InvokeNode *call;
            DBG(pgm.cc.comment("map_str_int_set"));
            pgm.cc.invoke(&call, imm(map_str_int_set),
                FuncSignature::build<void, void *, void *, int64_t>());
            call->setArg(0, obj_reg);
            call->setArg(1, idx_reg);
            call->setArg(2, val_op.as<x86::Gp>());
        }
    }
    // else: MadArray write not yet supported (no indexed set helper)
}

// Manage operands/registers for use on local as well as global variables
Operand &TokenCpnd::voperand(Program &pgm, Variable *var)
{
    var = canonical_scope_variable(this, var);
    std::map<Variable *, Operand>::iterator rmi;
    if ( var->is_global() && !var->data && !var->has_aot_data() )
	global_has_compilable_address(pgm, var);
    bool global_addrable = var->is_global() && (var->data || var->has_aot_data());

    DBG(pgm.cc.comment("TokenCpnd::voperand() on"));
    DBG(pgm.cc.comment(var->name.c_str()));

    if ( (rmi=operand_map.find(var)) != operand_map.end() )
    {
	DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::voperand(" << var->name << ") found" << std::endl);
	// Copy global variable to register — needs to happen every time we
	// access a global, including global constants (string literals etc.).
	// Without re-emitting for constants, the initial `mov reg, imm` that
	// populates the virtual register is emitted at the first use site,
	// which may be inside a conditional branch; subsequent uses on other
	// branches see the spilled slot uninitialized.
	// Fixed arrays still skip this: the register already holds the base
	// pointer (a program-lifetime constant), and movreg would reload the
	// numeric element zero as if it were the pointer value.
		if ( global_addrable && !var->is_fixed_array()
		  && rmi->second.isReg() )
		{
		    DBG(pgm.cc.comment("TokenCpnd::voperand() variable found, var->is_global() && var->data"));
		    movreg(pgm, rmi->second, var);
	        }
		else if ( global_addrable && var->is_fixed_array()
			  && rmi->second.isReg() )
		{
	    // Global fixed-arrays: cached Gp holds the base pointer (a
	    // program-lifetime constant). Re-emit the immediate load so
	    // every reuse — including ones on a branch the original mov
	    // doesn't dominate — sees the address. Without this, the
	    // first use's `mov reg, imm(addr)` is emitted inside one
	    // branch (e.g. the `then`) and the `else` branch reads an
	    // uninitialized vreg.
	    DBG(pgm.cc.comment("TokenCpnd::voperand() re-emit fixed-array base"));
	    pgm.emit_data_mov(rmi->second.as<x86::Gp>(), var);
        }
	else if ( var->is_fixed_array() && rmi->second.isReg()
		  && fixed_array_stack.count(var) )
	{
	    // Local (stack-backed) fixed-arrays: same re-emit story as
	    // the global case. The cached Gp holds the LEA of the stack
	    // slot, but the LEA was emitted at the first use site. A
	    // second use on a divergent branch reads an uninitialized
	    // vreg — concrete failure: SMAUG `bug()` declares
	    // `char buf[MAX_STRING_LENGTH]`, first uses it inside
	    // `if (fpArea != NULL) sprintf(buf, ...)`, then
	    // unconditionally `strcpy(buf, "[*****] BUG: ")` after. When
	    // fpArea is NULL the strcpy lands on NULL.
	    DBG(pgm.cc.comment("TokenCpnd::voperand() re-emit local-fixed-array LEA"));
	    pgm.cc.lea(rmi->second.as<x86::Gp>(), fixed_array_stack[var]);
	}
		else if ( global_addrable && rmi->second.isMem()
			  && ((var->type->basetype() == BaseType::btStruct
			    || var->type->basetype() == BaseType::btClass)
			   || is_large_simd_type(var->type)) )
	{
	    // Global structs / wide SIMD vectors: cached Mem uses a base register. Re-emit
	    // the absolute-address load into that base reg so the Mem
	    // is well-defined on every branch — same reasoning as the
	    // fixed-array case.
	    DBG(pgm.cc.comment("TokenCpnd::voperand() re-emit global aggregate base"));
	    x86::Mem mem = rmi->second.as<x86::Mem>();
	    if ( mem.hasBaseReg() )
	    {
		// We allocated the base as IntPtr (Gpq). Reconstruct.
		x86::Gpq base_gp(mem.baseId());
		pgm.emit_data_mov(base_gp, var);
	    }
        }
	return rmi->second;
    }

    // [&] capture: if this is a capturing lambda and var is from the outer scope, access it
    // through the env pointer rather than allocating a new stack slot
    if ( method && method->env_param && method->env_param != var )
    {
	FuncDef *fdef = (FuncDef *)method->returns.type;
	bool is_cap = false;
	for ( auto *cv : fdef->potential_captures )
	    if ( cv == var ) { is_cap = true; break; }
	if ( is_cap )
	{
	    // Find or assign capture index (in order of first access)
	    int cap_idx = -1;
	    for ( size_t ci = 0; ci < fdef->captures.size(); ++ci )
		if ( fdef->captures[ci].name == var->name ) { cap_idx = (int)ci; break; }
	    if ( cap_idx < 0 )
	    {
		cap_idx = (int)fdef->captures.size();
		FuncDef::CaptureEntry ce;
		ce.name = var->name;
		ce.type = var->type;
		fdef->captures.push_back(ce);
	    }
	    // Load env pointer (the hidden first param of this lambda)
	    Operand &env_op = voperand(pgm, method->env_param);
	    x86::Gp env_gp = pgm.cc.newIntPtr("__env_gp");
	    if ( env_op.isMem() )
		pgm.cc.mov(env_gp, env_op.as<x86::Mem>());
	    else
		env_gp = env_op.as<x86::Gp>();
	    // Build operand: numeric → direct Mem in env[cap_idx]; string → loaded pointer
	    if ( var->type->is_numeric() )
		operand_map[var] = x86::ptr(env_gp, (int64_t)cap_idx * 8, (uint32_t)var->type->size);
	    else
	    {
		x86::Gp str_ptr = pgm.cc.newIntPtr("%s", var->name.c_str());
		pgm.cc.mov(str_ptr, x86::qword_ptr(env_gp, (int64_t)cap_idx * 8));
		operand_map[var] = str_ptr;
	    }
	    var->flags |= vfREGSET;
	    return operand_map[var];
	}
    }

    DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::voperand(" << var->name << ") building register" << std::endl);
    // C99 variable-length array: `T name[runtime_expr];` → backed by a
    // stack-resident pointer slot whose value is `malloc(N*elem_size)`
    // emitted at scope entry. The matching free is emitted in cleanup().
    // Subscript / pointer-arith on `name` reuses the existing pointer
    // handling because `var->type` was already retyped to T*.
    if ( var->is_vla() )
    {
	DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(var->type);
	DataDef *elem_type = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;

	x86::Mem slot = pgm.cc.newStack(8, 8);
	slot.setSize(8);
	operand_map[var] = slot;
	var->flags |= vfSTACKSET;

	// Compile the size expression into a Gp.
	regdefp_t sz_rdp = {NULL, NULL, NULL};
	Operand &sz_op = var->vla_size_expr->compile(pgm, sz_rdp);
	x86::Gp sz_reg = pgm.cc.newGpq("__vla_n");
	if ( sz_op.isReg() && sz_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    // Widen 32-bit int result to 64-bit for malloc size argument.
	    if ( sz_op.as<x86::Gp>().isGpd() )
		pgm.cc.movsxd(sz_reg, sz_op.as<x86::Gp>());
	    else
		pgm.cc.mov(sz_reg, sz_op.as<x86::Gp>());
	}
	else if ( sz_op.isImm() )
	    pgm.cc.mov(sz_reg, sz_op.as<Imm>());
	else if ( sz_op.isMem() )
	    pgm.cc.mov(sz_reg, sz_op.as<x86::Mem>());
	else
	    throw "TokenCpnd::voperand VLA: size expression yields unsupported operand";

	if ( aggregate_has_runtime_size(elem_type) )
	{
	    x86::Gp elem_size_gp = emit_runtime_aggregate_size(pgm, elem_type, "__vla_elem");
	    pgm.cc.imul(sz_reg, elem_size_gp);
	}
	else
	{
	    size_t elem_size = elem_type->size ? elem_type->size : 8;
	    if ( elem_size > 1 )
		pgm.cc.imul(sz_reg, sz_reg, imm((int64_t)elem_size));
	}

	// malloc(total_bytes)
	x86::Gp buf = pgm.cc.newIntPtr("%s_vla", var->name.c_str());
	InvokeNode *call;
	pgm.cc.invoke(&call, imm((void *)::malloc),
		      FuncSignature::build<void *, size_t>());
	call->setArg(0, sz_reg);
	call->setRet(0, buf);

	pgm.cc.mov(slot, buf);
	var->flags |= vfREGSET;
	return operand_map[var];
    }
    // C fixed-size array: allocate stack slot (local) or load heap pointer
    // (global / static-local). The operand is the base pointer; subscript
    // code adds index*elem_size.
    if ( var->is_fixed_array() )
    {
	x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		if ( global_addrable )
		{
		    // Global or static-local: parseDeclaration calloc'd the backing
		    // storage. Load its absolute address.
	    DBG(pgm.cc.comment("voperand fixed-size array (global/static)"));
	    pgm.emit_data_mov(reg, var);
	}
	else
	{
	    size_t elem_size = var->type->size ? var->type->size : 8;
	    size_t total = elem_size * var->total_elements();
	    uint32_t align = (uint32_t)(var->type ? var->type->alignment() : 1);
	    if ( align == 0 )
		align = 1;
	    if ( align > 8 )
		align = 8;
	    DBG(pgm.cc.comment("voperand fixed-size array (stack)"));
	    x86::Mem stack = pgm.cc.newStack((uint32_t)total, align);
	    pgm.cc.lea(reg, stack);
	    fixed_array_stack[var] = stack;
	}
	operand_map[var] = reg;
	var->flags |= vfREGSET;
	return operand_map[var];
    }
    if ( global_addrable && var->type && var->type->is_simd() && var->type->size > 8 )
    {
	DBG(pgm.cc.comment("voperand global SIMD: load absolute base"));
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", var->name.c_str());
	pgm.emit_data_mov(base_reg, var);
	operand_map[var] = x86::ptr(base_reg, 0, (uint32_t)var->type->size);
	var->flags |= vfREGSET;
	return operand_map[var];
    }
    if ( (var->flags & vfPARAM) && is_large_simd_type(var->type) )
    {
	DBG(pgm.cc.comment("voperand param wide SIMD: incoming by-value copy pointer"));
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", var->name.c_str());
	operand_map[var] = base_reg;
	var->flags |= vfREGSET;
	return operand_map[var];
    }
    if ( global_addrable && is_large_simd_type(var->type) )
    {
	// Large SIMD globals (>128 bits): Mem-backed operand.
	// Can't fit in a single XMM register.
	DBG(pgm.cc.comment("voperand global wide SIMD: Mem-backed"));
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", var->name.c_str());
	pgm.emit_data_mov(base_reg, var);
	operand_map[var] = x86::ptr(base_reg, 0, (uint32_t)var->type->size);
	var->flags |= vfREGSET;
	return operand_map[var];
    }
	    if ( global_addrable
	      && (var->type->basetype() == BaseType::btStruct
	       || var->type->basetype() == BaseType::btClass)
	      && (var->type->type() == DataType::dtRESERVED || pgm.aot_tracking) )
    {
	// Mem operand for member access. In JIT mode, only user-defined
	// structs (dtRESERVED) need this; built-in opaque classes use Gp.
	// In AOT mode, ALL struct/class globals use Mem for operand
	// stability across large functions (re-emit via invalidation).
	DBG(pgm.cc.comment("voperand global struct/class: load absolute base"));
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", var->name.c_str());
	pgm.emit_data_mov(base_reg, var);
	operand_map[var] = x86::ptr(base_reg, 0, (uint32_t)var->type->size);
	var->flags |= vfREGSET;
	return operand_map[var];
    }
    // Ordinary local scalar numerics need stable storage too. Keeping them
    // only in transient registers lets calls / branches / loops observe stale
    // values once those regs are reused. Use stack-backed Mem operands for
    // non-pointer local numerics, the same way address-taken locals already do.
    if ( (var->flags & vfSTACK) && var->type->is_numeric()
      && (!var->type->is_pointer() || (var->flags & vfADDRTAKEN)) )
    {
	// Allocate at least 8 bytes for integer/pointer stack slots.
	// The JIT uses 64-bit (Gpq) registers throughout; writing a
	// Gpq to a 4-byte slot overwrites 4 bytes of adjacent stack.
	// GCC does the same — stack locals are pointer-aligned.
	uint32_t slot_size = (uint32_t)var->type->size;
	if ( var->type->is_integer() && slot_size < 8 )
	    slot_size = 8;
	uint32_t align = slot_size < 8 ? (uint32_t)var->type->size : 8;
	if ( align == 0 )
	    align = 8;
	x86::Mem stack = pgm.cc.newStack(slot_size, align);
	stack.setSize((uint32_t)var->type->size);
	operand_map[var] = stack;
	var->flags |= vfSTACKSET;

	if ( !(var->flags & vfPARAM) )
	{
	    // Emit the zero-init at the function prologue (before any
	    // branches) so it runs exactly once per function entry.
	    // Without this, the init lands at the first-use site which
	    // may be inside a conditional branch — re-zeroing the slot
	    // every time that branch is taken across loop iterations.
	    // Use a dedicated temporary whose lifetime is confined to
	    // the prologue region to avoid register-pressure interference
	    // with later code (e.g. union/struct brace-init memcpy).
	    BaseNode *saved_cursor = NULL;
	    if ( pgm.tkFunction && pgm.tkFunction->prologue_cursor )
		saved_cursor = pgm.cc.setCursor(pgm.tkFunction->prologue_cursor);

	    // Use a qword-sized mov-immediate to zero the entire slot.
	    // This avoids creating a virtual register whose lifetime
	    // could interfere with later code.
	    x86::Mem dst = stack;
	    if ( var->type->is_integer() )
	    {
		dst.setSize(8);
		pgm.cc.mov(dst, imm(0));
	    }
	    else if ( var->type->is_real() )
	    {
		dst.setSize(8);
		pgm.cc.mov(dst, imm(0));
	    }

	    if ( saved_cursor )
	    {
		pgm.tkFunction->prologue_cursor = pgm.cc.cursor();
		pgm.cc.setCursor(saved_cursor);
	    }
	}
	var->flags |= vfREGSET;
	return operand_map[var];
    }
    if ( (var->flags & vfSTACK) && !var->type->is_numeric() )
    {
	// Function parameters receive their value from setArg — just create
	// a Gp register to hold the incoming pointer.  No stack allocation
	// or construction; the caller owns the object.
	if ( var->flags & vfPARAM )
	{
	    DBG(pgm.cc.comment("voperand param (non-numeric) — bare register"));
	    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
	    operand_map[var] = reg;
	}
	else
	{
	DBG(pgm.cc.comment("voperand on stack and non-numeric"));
	switch(var->type->type())
	{
	    case DataType::dtSTRING:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::string), 4);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(std::cout << "TokenCpnd::voperand(" << var->name << ") stack var calling string_construct[" << (uint64_t)string_construct << ']' << std::endl);
		    DBG(pgm.cc.comment("string_construct"));
                    InvokeNode* call; pgm.cc.invoke(&call, imm(string_construct), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtSSTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::stringstream), 4);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(pgm.cc.comment("stringstream_construct"));
                    InvokeNode* call; pgm.cc.invoke(&call, imm(stringstream_construct), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtIFSTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::ifstream), 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(pgm.cc.comment("ifstream_construct"));
                    InvokeNode* call; pgm.cc.invoke(&call, imm(ifstream_construct), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtOFSTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::ofstream), 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(pgm.cc.comment("ofstream_construct"));
                    InvokeNode* call; pgm.cc.invoke(&call, imm(ofstream_construct), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtFSTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::fstream), 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(pgm.cc.comment("fstream_construct"));
                    InvokeNode* call; pgm.cc.invoke(&call, imm(fstream_construct), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtARRAY:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(MadArray), 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DBG(pgm.cc.comment("madarray_construct"));
                    InvokeNode* call; pgm.cc.invoke(&call, imm(madarray_construct), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtVECTOR:
		{
		    x86::Mem stack = pgm.cc.newStack(var->type->size, 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DataDefVECTOR *vdd = static_cast<DataDefVECTOR *>(var->type);
		    void *ctor = vdd->element_type->is_string()
			? (void *)vector_str_construct : (void *)vector_int_construct;
		    DBG(pgm.cc.comment("vector construct"));
		    InvokeNode* call; pgm.cc.invoke(&call, imm(ctor), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtMAP:
		{
		    x86::Mem stack = pgm.cc.newStack(var->type->size, 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DataDefMAP *mdd = static_cast<DataDefMAP *>(var->type);
		    void *ctor = mdd->val_type->is_string()
			? (void *)map_str_str_construct : (void *)map_str_int_construct;
		    InvokeNode* call; pgm.cc.invoke(&call, imm(ctor), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtSET:
		{
		    x86::Mem stack = pgm.cc.newStack(var->type->size, 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DataDefSET *sdd = static_cast<DataDefSET *>(var->type);
		    void *ctor = sdd->element_type->is_string()
			? (void *)set_str_construct : (void *)set_int_construct;
		    InvokeNode* call; pgm.cc.invoke(&call, imm(ctor), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtLIST:
		{
		    x86::Mem stack = pgm.cc.newStack(var->type->size, 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    DataDefLIST *ldd = static_cast<DataDefLIST *>(var->type);
		    void *ctor = ldd->element_type->is_string()
			? (void *)list_str_construct : (void *)list_int_construct;
		    InvokeNode* call; pgm.cc.invoke(&call, imm(ctor), FuncSignature::build<void *, void *>());
		    call->setArg(0, reg);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtISTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::istream), 8);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    operand_map[var] = reg;
		}
		break;
	    case DataType::dtOSTREAM:
		{
		    x86::Mem stack = pgm.cc.newStack(sizeof(std::ostream), 4);
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    pgm.cc.lea(reg, stack);
		    operand_map[var] = reg;
		}
		break;
	    default:
		if ( var->type->reftype() == RefType::rtReference
		||   var->type->reftype() == RefType::rtPointer )
		{
		    DBG(pgm.cc.comment("pgm.cc.newIntPtr"));
		    x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
		    operand_map[var] = reg;
		    break;
		}
		if ( var->type->basetype() == BaseType::btStruct
		||   var->type->basetype() == BaseType::btClass )
		{
		    // Static-local or global struct: backing store was calloc'd
		    // at parse time. Address it through a base register loaded
		    // with the absolute heap address, mimicking the
		    // fixed-array global path. Without this the struct is
		    // re-allocated on the stack every call and "static"
		    // semantics are lost — `static struct X x` becomes
		    // observably stack-resident and `&x` returns a stack
		    // address, breaking persistence across calls.
		    if ( global_addrable )
		    {
				DBG(pgm.cc.comment("voperand global struct: load absolute base"));
			x86::Gp base_reg = pgm.cc.newIntPtr("%s", var->name.c_str());
			pgm.emit_data_mov(base_reg, var);
			x86::Mem mem = x86::ptr(base_reg, 0, (uint32_t)var->type->size);
			operand_map[var] = mem;
			var->flags |= vfREGSET;
			return operand_map[var];
		    }
		    DataDefSTRUCT *dds = dynamic_cast<DataDefSTRUCT *>(var->type);
		    if ( dds && dds->has_runtime_size() )
		    {
			x86::Gp total_bytes = emit_runtime_struct_size(pgm, dds, (var->name + "_bytes").c_str());
			x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
			InvokeNode *call;
			pgm.cc.invoke(&call, imm((void *)::malloc),
			    FuncSignature::build<void *, size_t>());
			call->setArg(0, total_bytes);
			call->setRet(0, reg);
			operand_map[var] = reg;
			break;
		    }
		    // align stack to struct's max member alignment (C ABI compatible)
		    size_t struct_align = 8;
		    if ( dds )
			struct_align = dds->max_align;
		    x86::Mem stack = pgm.cc.newStack(var->type->size, (uint32_t)struct_align);
		    operand_map[var] = stack;

		    // Construct any non-trivial members (strings, streams) inside the struct
		    if ( !dds->members.empty() )
		    {
			x86::Gp base_reg = pgm.cc.newIntPtr("%s", (var->name + ".base").c_str());
			pgm.cc.lea(base_reg, stack);
			ssize_t ofs = 0;
			for ( auto &m : dds->members )
			{
			    if ( m.second->rawtype() == DataType::dtSTRING )
			    {
				x86::Gp mreg = pgm.cc.newIntPtr("%s", (var->name + "." + m.first).c_str());
				pgm.cc.lea(mreg, x86::ptr(base_reg, (int32_t)ofs));
				DBG(pgm.cc.comment(("struct member " + m.first + " string_construct").c_str()));
                                InvokeNode* call; pgm.cc.invoke(&call, imm(string_construct), FuncSignature::build<void *, void *>());
				call->setArg(0, mreg);
			    }
			    ofs += (ssize_t)m.second->size;
			}
		    }
		    // Track class objects for LIFO destructor ordering
		    if ( var->type->basetype() == BaseType::btClass )
		    {
			DataDefCLASS *ddc = static_cast<DataDefCLASS *>(var->type);
			if ( ddc->has_user_dtor )
			    destruct_order.push_back(var);
		    }
		    break;
		}
		std::cerr << "unsupported type: " << (int)var->type->type() << std::endl;
		std::cerr << "reftype: " << (int)var->type->reftype() << std::endl;
		throw "TokenCpnd()::voperand() unsupported type on stack";
		
	} // switch
    } // else (non-param stack variable)
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
	    movreg(pgm, rmi->second, var); // first initialization of non-stack register (regset)
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
	    if ( var->type->is_real() && rmi->second.isReg() && rmi->second.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.cc.xorps(rmi->second.as<x86::Xmm>(), rmi->second.as<x86::Xmm>()); // cerr << "WARNING: floating point not initialize by voperand()" << endl;
	}
    }
    var->flags |= vfREGSET;

    if ( rmi == operand_map.end() && (rmi=operand_map.find(var)) == operand_map.end() )
	throw "TokenCpnd::voperand() failure";
    return rmi->second;
}


// only used for global varibles -- move register back into variable data
void TokenCpnd::putreg(Program &pgm, Variable *var)
{
    var = canonical_scope_variable(this, var);
    // shortcut out if we can't work with this variable
    if ( !(global_has_compilable_address(pgm, var)
	&& (var->flags & vfREGSET) && (var->flags & vfMODIFIED) && var->type->is_numeric()) )
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
    DBG(pgm.cc.comment("TokenCpnd::putreg() calling cc.mov(var->data, reg)"));
    // Large SIMD globals use Mem-backed operands that point directly to
    // var->data — writes already go to the backing store, no write-back.
    if ( rmi->second.isMem() && is_large_simd_type(var->type) )
    {
	var->flags &= ~vfMODIFIED;
	return;
    }
    if ( pgm.aot_tracking && var->has_aot_data() )
    {
	x86::Gp base = pgm.cc.newIntPtr("%s_aot_store", var->name.c_str());
	pgm.emit_data_mov(base, var);
	x86::Mem dst = x86::ptr(base, 0, (uint32_t)var->type->size);
	pgm.safemov(dst, rmi->second, var->type, var->type);
    }
    else if ( rmi->second.isReg() && rmi->second.as<BaseReg>().isGroup(RegGroup::kGp) )
	var->type->movrval2mptr(pgm.cc, var->data, rmi->second.as<x86::Gp>());
    else if ( rmi->second.isReg() && rmi->second.as<BaseReg>().isGroup(RegGroup::kVec) )
	var->type->movxval2mptr(pgm.cc, var->data, rmi->second.as<x86::Xmm>());

    var->flags &= ~vfMODIFIED;
}

// cleanup function: will call destructors on all stack objects
void TokenCpnd::cleanup(Program &pgm)
{
    x86::Compiler &cc = pgm.cc;
    std::map<Variable *, Operand>::iterator rmi;

    DBG(std::cout << "TokenCpnd[" << (uint64_t)this << (method ? method->returns.name : "") << "]::cleanup()" << std::endl);

    // compile deferred statements in reverse (LIFO) order before destructors
    for ( auto it = deferred.rbegin(); it != deferred.rend(); ++it )
    {
	DBG(cc.comment("defer statement"));
	regdefp_t regdp = {NULL, NULL, NULL};
	(*it)->compile(pgm, regdp);
    }

    // Call user-defined destructors in reverse declaration order (LIFO).
    // User dtor body runs before member cleanup (matches C++ semantics).
    for ( auto it = destruct_order.rbegin(); it != destruct_order.rend(); ++it )
    {
	Variable *var = *it;
	if ( var->type->basetype() != BaseType::btClass )
	    continue;
	DataDefCLASS *ddc = static_cast<DataDefCLASS *>(var->type);
	if ( !ddc->has_user_dtor )
	    continue;
	auto omi = operand_map.find(var);
	if ( omi == operand_map.end() )
	    continue;
	std::string dtor_name = "~" + ddc->name;
	Variable *dtor_mvar = ddc->findMethod(dtor_name);
	if ( !dtor_mvar || !dtor_mvar->data )
	    continue;
	FuncDef *dtor_func = (FuncDef *)((Method *)dtor_mvar->data)->returns.type;
	if ( !dtor_func || !dtor_func->funcnode )
	    continue;
	DBG(cc.comment("user destructor call"));
	DBG(std::cout << "cleanup: calling destructor for " << var->name << std::endl);
	Operand &obj_reg = omi->second;
	x86::Gp this_ptr = cc.newIntPtr("__dtor_this");
	if ( obj_reg.isMem() )
	    cc.lea(this_ptr, obj_reg.as<x86::Mem>());
	else
	    cc.mov(this_ptr, obj_reg.as<x86::Gp>());
	InvokeNode *call;
	cc.invoke(&call, dtor_func->funcnode->label(),
		  FuncSignature::build<void, void *>());
	call->setArg(0, this_ptr);
	// Call base class destructor after derived (C++ order)
	DataDefCLASS *base = ddc->base_class;
	while ( base && base->has_user_dtor )
	{
	    std::string base_dtor_name = "~" + base->name;
	    Variable *base_dtor = base->findMethod(base_dtor_name);
	    if ( base_dtor && base_dtor->data )
	    {
		FuncDef *bdf = (FuncDef *)((Method *)base_dtor->data)->returns.type;
		if ( bdf && bdf->funcnode )
		{
		    DBG(cc.comment("base class destructor call"));
		    InvokeNode *bcall;
		    cc.invoke(&bcall, bdf->funcnode->label(),
			      FuncSignature::build<void, void *>());
		    bcall->setArg(0, this_ptr);
		}
	    }
	    base = base->base_class;
	}
    }

    for ( rmi = operand_map.begin(); rmi != operand_map.end(); ++rmi )
    {
	// VLA cleanup: free the malloc'd buffer. Reads the pointer slot,
	// invokes free, then continues so any other type-based dispatch
	// below can also fire if applicable.
	if ( rmi->first->is_vla() )
	{
	    Variable *var = rmi->first;
	    Operand &slot = rmi->second;
	    if ( !slot.isMem() )
		continue;
	    x86::Gp p = cc.newIntPtr("%s_vla_free", var->name.c_str());
	    cc.mov(p, slot.as<x86::Mem>());
	    InvokeNode *call;
	    cc.invoke(&call, imm((void *)::free),
		      FuncSignature::build<void, void *>());
	    call->setArg(0, p);
	    continue;
	}
	if ( (rmi->first->flags & vfSTACK) )
	{
	    Operand &reg = rmi->second;
	    // Don't destruct parameter objects — the caller owns them
	    if ( (rmi->first->flags & vfPARAM) )
		continue;
	    if ( (rmi->first->type->basetype() == BaseType::btStruct
		|| rmi->first->type->basetype() == BaseType::btClass)
	      && aggregate_has_runtime_size(rmi->first->type)
	      && !rmi->first->is_fixed_array() )
	    {
		if ( reg.isReg() && reg.as<BaseReg>().isGroup(RegGroup::kGp) )
		{
		    InvokeNode *call;
		    cc.invoke(&call, imm((void *)::free),
			FuncSignature::build<void, void *>());
		    call->setArg(0, reg.as<x86::Gp>());
		}
		continue;
	    }
	    if ( rmi->first->type->type() > DataType::dtRESERVED )
	    {
		Variable *var = rmi->first;

		switch(var->type->type())
		{
		    case DataType::dtSTRING:
			{
			    DBG(std::cout << "TokenCpnd::cleanup(" << var->name << ") calling string_destruct[" << (uint64_t)string_destruct << ']' << std::endl);
                            InvokeNode* call; cc.invoke(&call, imm(string_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, as_gp_ptr(pgm, reg, "str_dtor"));
			}
			break;
		    case DataType::dtSSTREAM:
			{
                            InvokeNode* call; cc.invoke(&call, imm(stringstream_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, as_gp_ptr(pgm, reg, "ss_dtor"));
			}
			break;
		    case DataType::dtARRAY:
			{
                            InvokeNode* call; cc.invoke(&call, imm(madarray_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, as_gp_ptr(pgm, reg, "arr_dtor"));
			}
			break;
		    case DataType::dtVECTOR:
			{
			    DataDefVECTOR *vdd = static_cast<DataDefVECTOR *>(var->type);
			    void *dtor = vdd->element_type->is_string()
				? (void *)vector_str_destruct : (void *)vector_int_destruct;
			    InvokeNode* call; cc.invoke(&call, imm(dtor), FuncSignature::build<void, void *>());
			    call->setArg(0, as_gp_ptr(pgm, reg, "vec_dtor"));
			}
			break;
		    case DataType::dtMAP:
			{
			    DataDefMAP *mdd = static_cast<DataDefMAP *>(var->type);
			    void *dtor = mdd->val_type->is_string()
				? (void *)map_str_str_destruct : (void *)map_str_int_destruct;
			    InvokeNode* call; cc.invoke(&call, imm(dtor), FuncSignature::build<void, void *>());
			    call->setArg(0, as_gp_ptr(pgm, reg, "map_dtor"));
			}
			break;
		    case DataType::dtSET:
			{
			    DataDefSET *sdd = static_cast<DataDefSET *>(var->type);
			    void *dtor = sdd->element_type->is_string()
				? (void *)set_str_destruct : (void *)set_int_destruct;
			    InvokeNode* call; cc.invoke(&call, imm(dtor), FuncSignature::build<void, void *>());
			    call->setArg(0, as_gp_ptr(pgm, reg, "set_dtor"));
			}
			break;
		    case DataType::dtLIST:
			{
			    DataDefLIST *ldd = static_cast<DataDefLIST *>(var->type);
			    void *dtor = ldd->element_type->is_string()
				? (void *)list_str_destruct : (void *)list_int_destruct;
			    InvokeNode* call; cc.invoke(&call, imm(dtor), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtIFSTREAM:
			{
                            InvokeNode* call; cc.invoke(&call, imm(ifstream_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtOFSTREAM:
			{
                            InvokeNode* call; cc.invoke(&call, imm(ofstream_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtFSTREAM:
			{
                            InvokeNode* call; cc.invoke(&call, imm(fstream_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtISTREAM:
			{
                            InvokeNode* call; cc.invoke(&call, imm(istream_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtOSTREAM:
			{
                            InvokeNode* call; cc.invoke(&call, imm(ostream_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    default:
			// For structs: destruct any non-trivial members
			if ( var->type->basetype() == BaseType::btStruct
			||   var->type->basetype() == BaseType::btClass )
			{
			    DataDefSTRUCT *dds = static_cast<DataDefSTRUCT *>(var->type);
			    if ( !dds->members.empty() && reg.isMem() )
			    {
				x86::Gp base_reg = cc.newIntPtr("%s", (var->name + ".base").c_str());
				cc.lea(base_reg, reg.as<x86::Mem>());
				ssize_t ofs = 0;
				for ( auto &m : dds->members )
				{
				    if ( m.second->rawtype() == DataType::dtSTRING )
				    {
					x86::Gp mreg = cc.newIntPtr("%s", (var->name + "." + m.first).c_str());
					cc.lea(mreg, x86::ptr(base_reg, (int32_t)ofs));
					DBG(std::cerr << "cleanup: " << var->name << '.' << m.first << " string_destruct" << std::endl);
                                        InvokeNode* call; cc.invoke(&call, imm(string_destruct), FuncSignature::build<void, void *>());
					call->setArg(0, mreg);
				    }
				    ofs += (ssize_t)m.second->size;
				}
			    }
			}
			else
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
	_operand = newScalarXmm(pgm, regdp.second, "setregdp_xmm");
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
    else
    // Pointer / fixed-array operand: propagate its pointer type. Without
    // this the result of `buf + n` (buf=char[N]) keeps `regdp.second` as
    // bare `char` and downstream arg-packing for variadic calls truncates
    // the 64-bit address to a single byte, since dtCHAR maps to addArgT<char>().
    if ( DataDef *lptr = effective_pointer_type_for_arith(pgm, left) )
	regdp.second = lptr;
    else if ( DataDef *rptr = effective_pointer_type_for_arith(pgm, right) )
	regdp.second = rptr;
    if ( !regdp.second )
	regdp.second = infer_numeric_type(left, right);
    if ( !regdp.second )
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
	    _operand = newScalarXmm(pgm, regdp.second, "_operand_Xmm_");
	    regdp.first = &_operand;
	}
	pgm.safemov(*regdp.first, foperate(), regdp.second);
	return *regdp.first;
    }
    if ( !regdp.second )
    {
	DBG(pgm.cc.comment("optimize() inferring folded type"));
	DataDef *folded_type = infer_numeric_type(left, right);
	if ( !folded_type || !folded_type->is_integer() )
	{
	    folded_type = datadef();
	    if ( !folded_type || !folded_type->is_integer() )
		folded_type = &ddINT;
	}
	regdp.second = folded_type;
    }
    if ( !regdp.first )
    {
	_operand = regdp.second->newreg(pgm.cc, "_cf_");
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




// comma operator: evaluate left for side effects, return right's value.
// parseExprStmt chains brace-less expression-statement commas like
// `++p, ++i;` into TokenComma nodes (left=first, right=rest), so the
// while body in `while (cond) e1, e2;` runs both side effects per iter.
Operand &TokenComma::compile(Program &pgm, regdefp_t &regdp)
{
    if ( left )
    {
	regdefp_t lrdp = {nullptr, nullptr, nullptr};
	left->compile(pgm, lrdp);
    }
    if ( !right )
	throw "TokenComma::compile() missing right operand";
    return right->compile(pgm, regdp);
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












/////////////////////////////////////////////////////////////////////////////
// comparison operators                                                    //
/////////////////////////////////////////////////////////////////////////////









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
    // function reference — emit function's entry point address
    // A real function has is_function()=true but is_numeric()=false
    if ( var.type->is_function() && !var.type->is_numeric() && var.data )
    {
	DBG(pgm.cc.comment("TokenVar::compile() function address"));
	Method *method = (Method *)var.data;
	FuncDef *func = (FuncDef *)method->returns.type;

	x86::Gp addr_gp = pgm.cc.newGpq("%s", var.name.c_str());
	if ( func->funcnode )
	    pgm.cc.lea(addr_gp, x86::ptr(func->funcnode->label()));
	else
	{
	    void *func_addr = method->x86code;
	    if ( !func_addr && pgm.is_dynamic_symbol_allowed(var.name) )
	    {
		func_addr = dlsym(RTLD_DEFAULT, var.name.c_str());
		if ( func_addr )
		{
		    method->x86code = func_addr;
		    pgm.external_symbol_map[reinterpret_cast<uintptr_t>(func_addr)] =
			external_symbol_export_name(var.name, func_addr);
		}
	    }
	    if ( func_addr )
	    {
		if ( pgm.aot_tracking )
		    pgm.emit_data_mov(addr_gp, func_addr);
		else
		    pgm.cc.mov(addr_gp, imm(func_addr));
	    }
	}

	if ( !regdp.second )
	    regdp.second = var.type;
	return emit_ir_value(pgm, IRValue::reg(addr_gp, var.type), regdp, _operand, var.type);
    }

    DBG(pgm.cc.comment("TokenVar::compile() reg = operand()"));
    Operand &reg = operand(pgm);

    // Reference parameter: auto-dereference — return Mem through the pointer
    if ( (var.flags & vfREFERENCE) && reg.isReg()
      && reg.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	DBG(pgm.cc.comment("TokenVar::compile() reference auto-deref"));
	DataDef *base_type = var.type->is_pointer()
	    ? static_cast<DataDefPTR *>(var.type)->base_type : var.type;
	uint32_t sz = (uint32_t)base_type->size;
	if ( sz == 0 ) sz = 8;
	x86::Mem deref = x86::ptr(reg.as<x86::Gp>(), 0, sz);
	_operand = deref;
	// Override datatype so downstream code sees the base type, not the pointer
	_datatype = base_type;
	regdp.second = base_type;
	regdp.first = &_operand;
	return _operand;
    }

    if ( var.is_fixed_array() && (reg.isReg() || reg.isMem()) )
    {
	DataDefPTR *ptr_type = pgm.getPointerType(var.type);
	if ( !regdp.second || regdp.second->is_pointer() )
	    regdp.second = ptr_type;
	return emit_ir_value(pgm, ir_from_operand(reg, ptr_type), regdp, _operand, ptr_type);
    }

    if ( !regdp.second )
	regdp.second = _datatype;

    if ( is_large_simd_type(var.type) && reg.isMem() )
    {
	_operand = reg;
	regdp.first = &_operand;
	regdp.second = var.type;
	return _operand;
    }

    // Object-like globals in AOT mode materialize as Mem so their backing
    // addresses remain patchable across large functions. As expression
    // values, though, strings/streams/struct-like objects are passed around
    // by address, not by loading the first machine word from the object.
    if ( reg.isMem() && !var.type->is_numeric() && !var.type->is_pointer() )
    {
	x86::Gp addr = pgm.cc.newIntPtr("%s_obj", var.name.c_str());
	pgm.cc.lea(addr, reg.as<x86::Mem>());
	_operand = addr;
	regdp.first = &_operand;
	return _operand;
    }

    DBG(cout << "TokenVar::compile() name=" << var.name << " regdp.second.name " << regdp.second->name << endl);

    if ( (var.type->is_numeric() || var.type->is_pointer()) && (reg.isMem() || reg.isReg() || reg.isImm()) )
	return emit_ir_value(pgm, ir_from_operand(reg, var.type), regdp, _operand, var.type);

    if ( regdp.first )
    {
	if ( !reg.equals(*regdp.first) && regdp.first != &reg )
	{
	    DBG(pgm.cc.comment("TokenVar::compile() safemov(*ret, reg)"));
	    pgm.safemov(*regdp.first, reg, regdp.second, var.type);
	}
	return *regdp.first;
    }
    if ( reg.x86RmSize() < 4 && reg.x86RmSize() < regdp.second->size )
    {
	DBG(cout << "TokenVar::compile() reg.x86RmSize() " << reg.x86RmSize() << " < regdp.second->size " << regdp.second->size << endl);
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

    if ( const DataDefSTRUCT::BitFieldInfo *bf = bitfield_info() )
    {
	if ( !reg.isMem() )
	    pgm.Throw(this) << "Bit-field member does not have memory storage" << flush;
	x86::Mem storage = reg.as<x86::Mem>();
	storage.setSize((uint32_t)bf->storage_size);
	x86::Gp value = emit_bitfield_load(pgm, storage, *bf, "member_bf");
	return emit_ir_value(pgm, IRValue::reg(value, _datatype),
	    regdp, _operand, _datatype);
    }

    if ( reg.isMem() && !_datatype->is_numeric() && !_datatype->is_pointer() )
    {
	x86::Gp addr = pgm.cc.newIntPtr("%s_obj", var.name.c_str());
	pgm.cc.lea(addr, reg.as<x86::Mem>());
	_operand = addr;
	regdp.first = &_operand;
	return _operand;
    }

    if ( (_datatype->is_numeric() || _datatype->is_pointer()) && (reg.isMem() || reg.isReg() || reg.isImm()) )
    {
	// Fixed-array member accessed without subscript: array-to-pointer decay.
	// operand() returned an LEA-computed Gp (64-bit address) but _datatype
	// is the element type (e.g. ddCHAR, 1 byte). Use a pointer type so
	// emit_ir_value doesn't truncate the 64-bit address.
	DataDef *emit_type = _datatype;
	if ( is_fixed_array_member() && reg.isReg() )
	{
	    emit_type = pgm.getPointerType(_datatype);
	    if ( !regdp.second || regdp.second == _datatype )
		regdp.second = emit_type;
	}
	return emit_ir_value(pgm, ir_from_operand(reg, emit_type), regdp, _operand, emit_type);
    }

    if ( regdp.first )
    {
	DBG(pgm.cc.comment("TokenMember::compile() safemov(*ret, reg)"));
	pgm.safemov(*regdp.first, reg, regdp.second);
	return *regdp.first;
    }

    regdp.first = &reg;
    return reg;
}

// *ptr dereference — load/store through pointer
Operand &TokenDeref::operand(Program &pgm)
{
    Operand &ptr_op = pgm.tkFunction->voperand(pgm, &var);
    x86::Gp ptr_gp;
    if ( ptr_op.isMem() )
    {
	// Address-taken pointer variables live in a stack Mem slot;
	// load the pointer value into a Gp before using it as the
	// base of the dereferenced Mem. Without this, `.as<Gp>()` on
	// a Mem silently reinterprets it and subsequent ptr(gp, 0, 8)
	// yields invalid encodings that asmjit flags at finalize.
	ptr_gp = pgm.cc.newIntPtr("%s", ("*" + var.name).c_str());
	pgm.cc.mov(ptr_gp, ptr_op.as<x86::Mem>());
    }
    else
	ptr_gp = ptr_op.as<x86::Gp>();
    // Numeric and pointer pointees both live in memory at [ptr]. Only
    // aggregate / non-scalar dereferences use the pointer value itself as
    // the address of the object.
    if ( deref_type->is_numeric() || deref_type->is_pointer() )
	_operand = x86::ptr(ptr_gp, 0, (uint32_t)deref_type->size);
    else
	_operand = ptr_gp; // non-scalar: pointer value IS the object address
    return _operand;
}

Operand &TokenDeref::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenDeref::compile()"));
    if ( !regdp.second )
	regdp.second = deref_type;

    Operand &mem = operand(pgm);

    if ( deref_type && deref_type->is_simd() )
    {
	// Wide SIMD (>16 bytes): Xmm can't hold the full value.  Copy
	// qword-by-qword into a stack buffer and return the Mem operand.
	if ( deref_type->size > 16 )
	{
	    x86::Mem buf = pgm.cc.newStack((uint32_t)deref_type->size, 16);
	    Operand dst = buf;
	    emit_raw_aggregate_copy(pgm, dst, mem, deref_type, "deref_wide_simd");
	    regdp.second = deref_type;
	    if ( regdp.first && regdp.first->isMem() )
	    {
		Operand caller_dst = *regdp.first;
		emit_raw_aggregate_copy(pgm, caller_dst, dst, deref_type, "deref_wide_simd_dst");
		return *regdp.first;
	    }
	    _operand = buf;
	    regdp.first = &_operand;
	    return _operand;
	}
	x86::Xmm xmm = deref_type->newreg(pgm.cc, "deref_simd").as<x86::Xmm>();
	if ( mem.isMem() )
	    load_mem_to_xmm(pgm, xmm, mem.as<x86::Mem>(), deref_type);
	else if ( mem.isReg() && mem.as<BaseReg>().isGroup(RegGroup::kVec) )
	    pgm.safemov(xmm, mem.as<x86::Xmm>(), deref_type, deref_type);
	else
	    throw "TokenDeref::compile() SIMD dereference expects memory or Xmm source";
	regdp.second = deref_type;
	if ( regdp.first )
	{
	    if ( regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec) )
	    {
		pgm.safemov(*regdp.first, xmm, deref_type, deref_type);
		return *regdp.first;
	    }
	    if ( regdp.first->isMem() )
	    {
		x86::Mem dst = regdp.first->as<x86::Mem>();
		store_xmm_to_mem(pgm, dst, xmm, deref_type);
		return *regdp.first;
	    }
	}
	_operand = xmm;
	regdp.first = &_operand;
	return _operand;
    }

    if ( (deref_type->is_numeric() || deref_type->is_pointer())
      && (mem.isMem() || mem.isReg() || mem.isImm()) )
	return emit_ir_value(pgm, ir_from_operand(mem, deref_type), regdp, _operand, deref_type);

    if ( regdp.first )
    {
	pgm.safemov(*regdp.first, mem, regdp.second);
	return *regdp.first;
    }

    regdp.first = &_operand;
    return _operand;
}

Operand &TokenDerefExpr::operand(Program &pgm)
{
    regdefp_t sub = {nullptr, nullptr, nullptr};
    Operand &ptr_op = expr->compile(pgm, sub);
    x86::Gp ptr_gp;
    if ( ptr_op.isMem() )
    {
	// Expression-produced pointers can live in a stack slot (for
	// example temporaries or address-taken pointer locals). Load the
	// pointer value before using it as the base of a dereferenced Mem;
	// reinterpreting a Mem as Gp produces invalid assignments at
	// asmjit finalize time.
	ptr_gp = pgm.cc.newIntPtr("*expr");
	pgm.cc.mov(ptr_gp, ptr_op.as<x86::Mem>());
    }
    else
    if ( ptr_op.isImm() )
    {
	ptr_gp = pgm.cc.newIntPtr("*expr_imm");
	pgm.cc.mov(ptr_gp, ptr_op.as<Imm>());
    }
    else
	ptr_gp = ptr_op.as<x86::Gp>();
    if ( deref_type->is_numeric() || deref_type->is_pointer() )
	_operand = x86::ptr(ptr_gp, 0, (uint32_t)deref_type->size);
    else
	_operand = ptr_gp;
    return _operand;
}

Operand &TokenDerefExpr::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenDerefExpr::compile()"));
    if ( !regdp.second )
	regdp.second = deref_type;

    Operand &mem = operand(pgm);

    if ( deref_type && deref_type->is_simd() )
    {
	x86::Xmm xmm = deref_type->newreg(pgm.cc, "derefexpr_simd").as<x86::Xmm>();
	if ( mem.isMem() )
	    load_mem_to_xmm(pgm, xmm, mem.as<x86::Mem>(), deref_type);
	else if ( mem.isReg() && mem.as<BaseReg>().isGroup(RegGroup::kVec) )
	    pgm.safemov(xmm, mem.as<x86::Xmm>(), deref_type, deref_type);
	else
	    throw "TokenDerefExpr::compile() SIMD dereference expects memory or Xmm source";
	regdp.second = deref_type;
	if ( regdp.first )
	{
	    if ( regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec) )
	    {
		pgm.safemov(*regdp.first, xmm, deref_type, deref_type);
		return *regdp.first;
	    }
	    if ( regdp.first->isMem() )
	    {
		x86::Mem dst = regdp.first->as<x86::Mem>();
		store_xmm_to_mem(pgm, dst, xmm, deref_type);
		return *regdp.first;
	    }
	}
	_operand = xmm;
	regdp.first = &_operand;
	return _operand;
    }

    if ( (deref_type->is_numeric() || deref_type->is_pointer())
      && (mem.isMem() || mem.isReg() || mem.isImm()) )
	return emit_ir_value(pgm, ir_from_operand(mem, deref_type), regdp, _operand, deref_type);

    if ( regdp.first )
    {
	pgm.safemov(*regdp.first, mem, regdp.second);
	return *regdp.first;
    }

    regdp.first = &_operand;
    return _operand;
}







// & address-of operator: emit LEA to get address of variable
Operand &TokenAddrOf::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenAddrOf::compile()"));
    Operand &obj = pgm.tkFunction->voperand(pgm, &var);

    x86::Gp addr = pgm.cc.newIntPtr("%s", ("&" + var.name).c_str());
    if ( var.is_global() && var.data && !var.is_fixed_array() )
	pgm.emit_data_mov(addr, &var);
    else
    if ( obj.isMem() )
	pgm.cc.lea(addr, obj.as<x86::Mem>());
    else
	pgm.cc.mov(addr, obj.as<x86::Gp>()); // already a pointer/register

    return emit_ir_value(pgm, IRValue::reg(addr, ptr_type), regdp, _operand, ptr_type);
}

Operand &TokenAddrExpr::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenAddrExpr::compile()"));

    x86::Gp addr = pgm.cc.newIntPtr("addr_expr");
    // TokenMember inherits from TokenVar, but its address must come from the
    // member lvalue operand, not the detached member symbol.
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(expr) )
    {
	if ( tm->is_bitfield_member() )
	    pgm.Throw(expr) << "Cannot take address of bit-field member" << flush;
	Operand &obj = expr->operand(pgm);
	if ( obj.isMem() )
	    pgm.cc.lea(addr, obj.as<x86::Mem>());
	else
	    pgm.cc.mov(addr, obj.as<x86::Gp>());
    }
    else if ( TokenComplexPart *tcp = dynamic_cast<TokenComplexPart *>(expr) )
    {
	Operand &obj = tcp->operand(pgm);
	if ( obj.isMem() )
	    pgm.cc.lea(addr, obj.as<x86::Mem>());
	else
	    pgm.cc.mov(addr, obj.as<x86::Gp>());
    }
    else if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
    {
	Variable &v = tv->var;
	Operand &obj = pgm.tkFunction->voperand(pgm, &v);
	if ( v.is_global() && v.data && !v.is_fixed_array() )
	    pgm.emit_data_mov(addr, &v);
	else if ( obj.isMem() )
	    pgm.cc.lea(addr, obj.as<x86::Mem>());
	else
	    pgm.cc.mov(addr, obj.as<x86::Gp>());
    }
    else if ( TokenSubscript *ts = dynamic_cast<TokenSubscript *>(expr) )
    {
	// &arr[i] — compute base + i*sizeof(elem) directly. expr->operand
	// would otherwise call compile() which loads the value, not the
	// address. Mirrors the address calculation in TokenSubscript::compile
	// (fixed-array path) without the trailing emit_ir_value load.
	Variable &obj_var = ts->object;
	Operand &obj_op = pgm.tkFunction->voperand(pgm, &obj_var);
	x86::Gp obj_reg = pgm.cc.newIntPtr("addr_sub_base");
	if ( obj_op.isMem() )
	{
	    if ( obj_var.is_fixed_array() )
		pgm.cc.lea(obj_reg, obj_op.as<x86::Mem>());
	    else
		pgm.cc.mov(obj_reg, obj_op.as<x86::Mem>());
	}
	else
	    pgm.cc.mov(obj_reg, obj_op.as<x86::Gp>());

	regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
	Operand &idx_op = ts->index->compile(pgm, idx_rdp);
	x86::Gp idx_reg = pgm.cc.newGpq("addr_sub_idx");
	load_idx_to_gpq(pgm, idx_reg, idx_op);

	if ( obj_var.is_fixed_array() )
	{
	    size_t consumed_dims = 1 + ts->extra_indices.size();
	    DataDef *result_type = fixed_array_subscript_result_type(obj_var, consumed_dims);
	    size_t elem_size = fixed_array_subscript_stride(obj_var, consumed_dims);
	    for ( size_t k = 0; k < ts->extra_indices.size(); ++k )
	    {
		uint32_t dim_k = obj_var.dims[k + 1];
		pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)dim_k));
		regdefp_t ex_rdp = {nullptr, nullptr, nullptr};
		Operand &ex_op = ts->extra_indices[k]->compile(pgm, ex_rdp);
		if ( ex_op.isImm() )
		    pgm.cc.add(idx_reg, ex_op.as<Imm>());
		else
		{
		    x86::Gp ex_widened = pgm.cc.newGpq("addr_sub_exidx");
		    load_idx_to_gpq(pgm, ex_widened, ex_op);
		    pgm.cc.add(idx_reg, ex_widened);
		}
	    }
	    uint32_t shift = scale_index_by_element_size(pgm, idx_reg, result_type, "addr_sub_size");
	    x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
	    pgm.cc.lea(addr, elem_mem);
	    return emit_ir_value(pgm, IRValue::reg(addr, ptr_type), regdp, _operand, ptr_type);
	}

	if ( obj_var.type && obj_var.type->is_string() )
	{
	    InvokeNode *call;
	    pgm.cc.invoke(&call, imm(string_cstr), FuncSignature::build<const char *, void *>());
	    call->setArg(0, obj_reg);
	    x86::Gp cstr = pgm.cc.newIntPtr("addr_sub_cstr");
	    call->setRet(0, cstr);
	    obj_reg = cstr;
	}

	size_t elem_size = 8;
	if ( obj_var.type && obj_var.type->is_string() )
	    elem_size = 1;
	else if ( obj_var.type->is_pointer() )
	{
	    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(obj_var.type);
	    DataDef *base = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
	    if ( base->size ) elem_size = base->size;
	    uint32_t shift = scale_index_by_element_size(pgm, idx_reg, base, "addr_sub_size");
	    pgm.cc.lea(addr, x86::ptr(obj_reg, idx_reg, shift, 0));
	    return emit_ir_value(pgm, IRValue::reg(addr, ptr_type), regdp, _operand, ptr_type);
	}

	if ( elem_size == 1 || elem_size == 2 || elem_size == 4 || elem_size == 8 )
	{
	    uint32_t shift = (elem_size == 8) ? 3
			    : (elem_size == 4) ? 2
			    : (elem_size == 2) ? 1 : 0;
	    pgm.cc.lea(addr, x86::ptr(obj_reg, idx_reg, shift, 0));
	}
	else
	{
	    pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)elem_size));
	    pgm.cc.lea(addr, x86::ptr(obj_reg, idx_reg, 0, 0));
	}
    }
    else
    {
	Operand &obj = expr->operand(pgm);
	if ( obj.isMem() )
	    pgm.cc.lea(addr, obj.as<x86::Mem>());
	else
	    pgm.cc.mov(addr, obj.as<x86::Gp>());
    }

    return emit_ir_value(pgm, IRValue::reg(addr, ptr_type), regdp, _operand, ptr_type);
}

Operand &TokenLabelAddr::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenLabelAddr::compile()"));
    asmjit::Label &L = lookup_or_make_label(pgm, name);
    x86::Gp addr = pgm.cc.newIntPtr("%s", name.c_str());
    pgm.cc.lea(addr, x86::ptr(L));
    return emit_ir_value(pgm, IRValue::reg(addr, ptr_type), regdp, _operand, ptr_type);
}

// va_arg(ap, type) — read next variadic argument from packed buffer and advance
Operand &TokenVaArg::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenVaArg::compile()"));

    // get the va_list operand — either from the variable (simple case)
    // or from the expression (subscript, dereference, etc.)
    Operand ap_storage;
    Operand *ap_op_ptr = NULL;
    // When the parser extracted ap_var from a TokenDeref (*pap), ap_var
    // is the pointer variable, not the va_list itself. We need to
    // dereference it: load [ap_var] to get the va_list value, and write
    // back through [ap_var] after advancing.
    bool ap_is_indirect = false;
    x86::Gp ap_indirect_addr;
    if ( ap_var )
    {
	ap_op_ptr = &pgm.tkFunction->voperand(pgm, ap_var);
	if ( ap_expr && dynamic_cast<TokenDeref *>(ap_expr) )
	{
	    ap_is_indirect = true;
	    ap_indirect_addr = pgm.cc.newIntPtr("va_indir_addr");
	    Operand &pval = *ap_op_ptr;
	    if ( pval.isReg() )
		pgm.cc.mov(ap_indirect_addr, pval.as<x86::Gp>());
	    else
		pgm.cc.mov(ap_indirect_addr, pval.as<x86::Mem>());
	    // ap_op becomes [pointer_value] — the va_list at the pointed-to location
	    ap_storage = x86::qword_ptr(ap_indirect_addr);
	    ap_op_ptr = &ap_storage;
	}
    }
    else if ( ap_expr )
    {
	// For struct member (a.g) or subscript (aps[4]) va_list expressions,
	// use operand() to get the Mem location so write-back after va_arg
	// updates the actual memory, not a dead register copy.
	if ( dynamic_cast<TokenMember *>(ap_expr)
	  || dynamic_cast<TokenSubscriptExpr *>(ap_expr) )
	{
	    Operand &loc = ap_expr->operand(pgm);
	    ap_storage = loc;
	}
	else
	{
	    regdefp_t ap_rdp = {NULL, NULL, NULL};
	    Operand &expr_op = ap_expr->compile(pgm, ap_rdp);
	    ap_storage = expr_op;
	}
	ap_op_ptr = &ap_storage;
    }
    else
	throw "TokenVaArg::compile() no ap_var or ap_expr";
    Operand &ap_op = *ap_op_ptr;

    // load current pointer value into a temp register
    x86::Gp ptr = pgm.cc.newIntPtr("va_ptr");
    if ( ap_op.isReg() )
	pgm.cc.mov(ptr, ap_op.as<x86::Gp>());
    else
	pgm.cc.mov(ptr, ap_op.as<x86::Mem>());

    if ( target_type && !target_type->is_numeric() && !target_type->is_pointer() && !target_type->is_real() )
    {
	bool runtime_sized = aggregate_has_runtime_size(target_type);
	Operand src_storage;
	Operand *src_ptr = NULL;
	x86::Mem slot;
	if ( runtime_sized )
	{
	    x86::Gp dyn_src = pgm.cc.newIntPtr("__va_arg_dyn");
	    pgm.cc.mov(dyn_src, x86::qword_ptr(ptr));
	    src_storage = dyn_src;
	    src_ptr = &src_storage;
	}
	else
	{
	    slot = pgm.cc.newStack((uint32_t)target_type->size, 8);
	    slot.setSize((uint32_t)target_type->size);
	    emit_raw_aggregate_copy(pgm, slot, ptr, target_type, "__va_arg_copy");
	    src_storage = as_gp_ptr(pgm, slot, "__va_arg_val");
	    src_ptr = &src_storage;
	}

	size_t advance = internal_vararg_static_slot_size(target_type);
	pgm.cc.add(ptr, (int64_t)advance);
	if ( ap_op.isReg() )
	    pgm.cc.mov(ap_op.as<x86::Gp>(), ptr);
	else
	    pgm.cc.mov(ap_op.as<x86::Mem>(), ptr);
	if ( ap_var && ap_var->is_global() && ap_var->data && !ap_is_indirect )
	{
	    x86::Gp data_addr = pgm.cc.newIntPtr("va_data_addr");
	    pgm.emit_data_mov(data_addr, ap_var);
	    pgm.cc.mov(x86::qword_ptr(data_addr), ptr);
	}

	if ( !regdp.second )
	    regdp.second = target_type;
	if ( regdp.first )
	{
	    if ( regdp.first->isMem() )
	    {
		if ( runtime_sized )
		{
		    x86::Gp size_gp = emit_runtime_aggregate_size(pgm, target_type, "__va_arg_size");
		    emit_runtime_aggregate_copy(pgm, *regdp.first, *src_ptr, size_gp, "__va_arg_store");
		}
		else
		    emit_raw_aggregate_copy(pgm, *regdp.first, *src_ptr, target_type, "__va_arg_store");
		return *regdp.first;
	    }
	    if ( regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kGp) )
	    {
		pgm.cc.mov(regdp.first->as<x86::Gp>(), src_ptr->as<x86::Gp>());
		return *regdp.first;
	    }
	}
	_operand = src_ptr->as<x86::Gp>();
	regdp.first = &_operand;
	return _operand;
    }

    // read value at [ptr] into result register
    x86::Gp result = pgm.cc.newGpq("va_val");
    pgm.cc.mov(result, x86::qword_ptr(ptr));

    // advance ap by 8 bytes and write back to the va_list variable
    pgm.cc.add(ptr, 8);
    if ( ap_op.isReg() )
	pgm.cc.mov(ap_op.as<x86::Gp>(), ptr);
    else
	pgm.cc.mov(ap_op.as<x86::Mem>(), ptr);
    // For global va_list variables, the register is a cached copy —
    // also write through to the backing storage so subsequent
    // va_arg calls (possibly in other functions) see the update.
    // Skip when indirect (ap_is_indirect): ap_var is the pointer
    // variable (*pap), not the va_list itself — writing ptr to pap
    // would overwrite the pointer-to-va_list with the buffer value.
    if ( ap_var && ap_var->is_global() && ap_var->data && !ap_is_indirect )
    {
	x86::Gp data_addr = pgm.cc.newIntPtr("va_data_addr");
	pgm.emit_data_mov(data_addr, ap_var);
	pgm.cc.mov(x86::qword_ptr(data_addr), ptr);
    }

    if ( !regdp.second )
	regdp.second = target_type;

    // For real types (double/float), the buffer holds IEEE 754 bits in a
    // Gp register. Reinterpret via movq into an Xmm so downstream code
    // sees a proper floating-point value instead of raw integer bits.
    if ( target_type && target_type->is_real() )
    {
	x86::Xmm xmm_result = pgm.cc.newXmm("va_dbl");
	pgm.cc.movq(xmm_result, result);
	return emit_ir_value(pgm, IRValue::reg(xmm_result, target_type), regdp, _operand, target_type);
    }
    return emit_ir_value(pgm, IRValue::reg(result, &ddINT64), regdp, _operand, target_type);
}

// load double into operand
Operand &TokenReal::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenReal::compile()"));
    if ( _datatype && _datatype->is_complex() )
    {
	_const = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, _val);
	_const.setSize(sizeof(double));
	return emit_complex_from_scalar(pgm, _const, &ddDOUBLE, _datatype,
					/*imag_value=*/true, regdp, _operand,
					"__imag_real");
    }
    if ( !regdp.second )
    {
	if ( !_datatype ) { throw "TokenReal has NULL _datatype"; }
	regdp.second = _datatype;
	DBG(pgm.cc.comment("TokenReal::compile() setting _datatype to double"));
    }
    DataDef *effective_type = (regdp.second && regdp.second->is_real())
	? regdp.second : _datatype;
    if ( effective_type->size == sizeof(float) )
    {
	_const = pgm.cc.newFloatConst(ConstPoolScope::kLocal, (float)_val);
	_const.setSize(sizeof(float));
    }
    else
    {
	_const = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, _val);
	_const.setSize(sizeof(double));
    }
    DBG(pgm.cc.comment("TokenReal::compile() emitting through IRBuilder"));
    DBG(cout << "TokenReal::compile() emitting through IRBuilder const[" << _val << "]" << endl);
    return emit_ir_value(pgm, IRValue::mem(_const, effective_type), regdp, _operand, effective_type);
}


// load integer into register
Operand &TokenInt::compile(Program &pgm, regdefp_t &regdp)
{
    if ( _datatype && _datatype->is_complex() )
    {
	_operand = imm(_token);
	return emit_complex_from_scalar(pgm, _operand, &ddINT64, _datatype,
					/*imag_value=*/true, regdp, _operand,
					"__imag_int");
    }
    if ( !regdp.second )
    {
	if ( !_datatype ) { throw "TokenInt has NULL _datatype"; }
	regdp.second = _datatype;
	DBG(pgm.cc.comment("TokenInt::compile() setting _datatype to int"));
    }
    DBG(cout << "TokenInt::compile[" << (uint64_t)this << "]() value: " << (int)_token << endl);
    return emit_ir_value(pgm, IRValue::imm(Imm(_token), _datatype), regdp, _operand, _datatype);
}

Operand &TokenChar::operand(Program &pgm)
{
    _operand = imm(_token);
    return _operand;
}

Operand &TokenChar::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !regdp.second )
        regdp.second = _datatype;
    DBG(cout << "TokenChar::compile[" << (uint64_t)this << "]() value: " << (char)_token << endl);
    return emit_ir_value(pgm, IRValue::imm(Imm(_token), _datatype), regdp, _operand, _datatype);
}

