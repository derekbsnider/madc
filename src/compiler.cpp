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

using namespace std;
using namespace asmjit;

static void bind_call_return(Program &pgm, InvokeNode *call, Operand *dest, DataDef *ret_type,
			     Operand &fallback_operand, bool is_variadic, bool narrow_int_ret=false);
const char *string_cstr(void *ptr);
extern "C" void *__madc_jmpbuf_for(void *user_buf);
extern "C" void __madc_builtin_longjmp(void *user_buf, int value);

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

extern "C" long madc_builtin_imaxabs(long x)
{
    return x < 0 ? -x : x;
}

extern "C" unsigned int madc_builtin_uabs(int x)
{
    return x < 0 ? -(unsigned int)x : (unsigned int)x;
}

extern "C" unsigned long madc_builtin_ulabs(long x)
{
    return x < 0 ? -(unsigned long)x : (unsigned long)x;
}

extern "C" unsigned long long madc_builtin_ullabs(long long x)
{
    return x < 0 ? -(unsigned long long)x : (unsigned long long)x;
}

extern "C" unsigned long madc_builtin_umaxabs(long x)
{
    return x < 0 ? -(unsigned long)x : (unsigned long)x;
}

// Materialize an operand into a Gp register. If already Gp, return it.
// If Mem (global struct/class), LEA the address into a fresh Gp.
static x86::Gp as_gp_ptr(Program &pgm, Operand &op, const char *name = "mem_ptr")
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

static asmjit::Label &lookup_or_make_label(Program &pgm, const std::string &name);

static x86::Gp materialize_gp_ptr_arg(Program &pgm, Operand &op, const char *name)
{
    x86::Gp src = as_gp_ptr(pgm, op, name);
    x86::Gp arg = pgm.cc.newIntPtr("%s_arg", name);
    pgm.cc.mov(arg, src);
    return arg;
}

static void emit_struct_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
    DataDefSTRUCT *dds, const std::vector<TokenBase *> &inits, TokenBase *err_loc);
static void emit_simd_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
    DataDefSIMD *vdd, const std::vector<TokenBase *> &inits, TokenBase *err_loc);
static void emit_raw_aggregate_copy(Program &pgm, Operand &dst, Operand &src,
				    DataDef *copy_type, const char *name);
static x86::Gp emit_runtime_aggregate_size(Program &pgm, DataDef *copy_type, const char *name);
static void emit_runtime_aggregate_copy(Program &pgm, Operand &dst, Operand &src,
					x86::Gp &size_gp, const char *name);
static size_t internal_vararg_static_slot_size(DataDef *type);
static void load_idx_to_gpq(Program &pgm, asmjit::x86::Gp &dst, asmjit::Operand &src);
static void load_mem_to_gpq(Program &pgm, x86::Gp &gp, const x86::Mem &mem, DataDef *type);
static void load_mem_to_xmm(Program &pgm, x86::Xmm &xmm, const x86::Mem &mem, DataDef *type);
static void store_xmm_to_mem(Program &pgm, x86::Mem &mem, x86::Xmm &xmm, DataDef *type);
static DataDef *infer_numeric_type(TokenBase *left, TokenBase *right);
static const char *token_constant_cstring(TokenBase *token);
static bool try_eval_const_i64(TokenBase *tb, int64_t &out);
static int64_t estimate_object_size(TokenBase *expr, int mode);

static bool is_direct_abs_builtin_name(const std::string &name)
{
    return name == "abs"
	|| name == "labs"
	|| name == "llabs"
	|| name == "imaxabs"
	|| name == "uabs"
	|| name == "ulabs"
	|| name == "ullabs"
	|| name == "umaxabs"
	|| name == "__builtin_abs"
	|| name == "__builtin_labs"
	|| name == "__builtin_llabs"
	|| name == "__builtin_imaxabs"
	|| name == "__builtin_uabs"
	|| name == "__builtin_ulabs"
	|| name == "__builtin_ullabs"
	|| name == "__builtin_umaxabs";
}

static bool direct_abs_builtin_disabled(Program &pgm, const std::string &name)
{
    if ( name.compare(0, 10, "__builtin_") == 0 )
	return false;
    return pgm.is_builtin_disabled(name);
}

static DataDef *direct_abs_builtin_type(const std::string &name)
{
    if ( name == "abs" || name == "__builtin_abs" )
	return &ddINT32;
    if ( name == "uabs" || name == "__builtin_uabs" )
	return &ddUINT32;
    if ( name == "ulabs" || name == "__builtin_ulabs" || name == "umaxabs" || name == "__builtin_umaxabs" )
	return &ddUINT64;
    if ( name == "ullabs" || name == "__builtin_ullabs" )
	return &ddUINT64;
    return &ddINT64;
}

static bool direct_abs_builtin_returns_32bit(const std::string &name)
{
    return name == "abs"
	|| name == "__builtin_abs"
	|| name == "uabs"
	|| name == "__builtin_uabs";
}

static void *direct_abs_builtin_target(const std::string &name)
{
    if ( name == "abs" || name == "__builtin_abs" )
	return (void *)(intptr_t)(int(*)(int))::abs;
    if ( name == "labs" || name == "__builtin_labs" )
	return (void *)(intptr_t)(long(*)(long))::labs;
    if ( name == "llabs" || name == "__builtin_llabs" )
	return (void *)(intptr_t)(long long(*)(long long))::llabs;
    if ( name == "imaxabs" || name == "__builtin_imaxabs" )
	return (void *)(intptr_t)(long(*)(long))madc_builtin_imaxabs;
    if ( name == "uabs" || name == "__builtin_uabs" )
	return (void *)(intptr_t)(unsigned int(*)(int))madc_builtin_uabs;
    if ( name == "ulabs" || name == "__builtin_ulabs" )
	return (void *)(intptr_t)(unsigned long(*)(long))madc_builtin_ulabs;
    if ( name == "ullabs" || name == "__builtin_ullabs" )
	return (void *)(intptr_t)(unsigned long long(*)(long long))madc_builtin_ullabs;
    if ( name == "umaxabs" || name == "__builtin_umaxabs" )
	return (void *)(intptr_t)(unsigned long(*)(long))madc_builtin_umaxabs;
    return (void *)(intptr_t)(long long(*)(long long))::llabs;
}

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

static bool try_eval_const_i64(TokenBase *tb, int64_t &out)
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

static int64_t estimate_object_size(TokenBase *expr, int mode)
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

static Operand &redirect_builtin_call(Program &pgm, const std::string &target_name,
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

static Operand &redirect_overflow_predicate_call(Program &pgm, const std::string &target_name,
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
		datatype_vec_t{DataType::dtINT32, DataType::dtUINT64, DataType::dtUINT64, DataType::dtUINT64},
		(fVOIDFUNC)sym);
	    if ( !target )
		target = pgm.findVariable(lookup);
	}
    }
    if ( !target || !target->type || !target->type->is_function() )
	pgm.Throw(site) << "missing overflow helper for " << site->var.name << flush;
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

static std::string runtime_symbol_name(const Variable &var)
{
    return var.storage_alias_name.empty() ? var.name : var.storage_alias_name;
}

static const char *simple_libc_builtin_redirect_target(const std::string &name)
{
    struct Redirect
    {
	const char *builtin;
	const char *target;
    };
    static const Redirect redirects[] = {
	{ "__builtin_printf", "printf" },
	{ "__builtin_fprintf", "fprintf" },
	{ "__builtin_sprintf", "sprintf" },
	{ "__builtin_snprintf", "snprintf" },
	{ "__builtin_printf_unlocked", "printf_unlocked" },
	{ "__builtin_fprintf_unlocked", "fprintf_unlocked" },
	{ "__builtin_putchar", "putchar" },
	{ "__builtin_puts", "puts" },
	{ "__builtin_fputc", "fputc" },
	{ "__builtin_fputs", "fputs" },
	{ "__builtin_fputs_unlocked", "fputs_unlocked" },
	{ "__builtin_fwrite", "fwrite" },
	{ NULL, NULL }
    };
    for ( const Redirect *it = redirects; it->builtin; ++it )
	if ( name == it->builtin )
	    return it->target;
    return NULL;
}

static bool is_overflow_predicate_helper_name(const std::string &name)
{
    return name == "__madc_add_overflow_p"
	|| name == "__madc_sub_overflow_p"
	|| name == "__madc_mul_overflow_p";
}

static bool is_overflow_store_helper_name(const std::string &name)
{
    return name == "__madc_add_overflow"
	|| name == "__madc_sub_overflow"
	|| name == "__madc_mul_overflow";
}

static std::string overflow_predicate_helper_suffix(DataDef *indicator_type)
{
    if ( !indicator_type || !indicator_type->is_integer() )
	return "s64";
    size_t size = indicator_type->size;
    if ( size <= 2 )
	return indicator_type->is_unsigned() ? "u16" : "s16";
    if ( size <= 4 )
	return indicator_type->is_unsigned() ? "u32" : "s32";
    return indicator_type->is_unsigned() ? "u64" : "s64";
}

static DataDef *overflow_predicate_indicator_type(const std::vector<TokenBase *> &parameters)
{
    if ( parameters.size() < 3 )
	return NULL;
    DataDef *indicator_type = parameters[2] ? parameters[2]->datadef() : NULL;
    if ( dynamic_cast<TokenInt *>(parameters[2]) != NULL && indicator_type == &ddINT )
	return infer_numeric_type(parameters[0], parameters[1]);
    return indicator_type;
}

static std::string typed_overflow_predicate_symbol_name(const Variable &var,
							const std::vector<TokenBase *> &parameters)
{
    if ( !is_overflow_predicate_helper_name(var.name) || parameters.size() < 3 )
	return runtime_symbol_name(var);
    DataDef *indicator_type = overflow_predicate_indicator_type(parameters);
    return var.name + "_" + overflow_predicate_helper_suffix(indicator_type);
}

static DataDef *overflow_store_result_type(const std::vector<TokenBase *> &parameters)
{
    if ( parameters.size() < 3 || !parameters[2] )
	return NULL;
    DataDefPTR *ptr_type = dynamic_cast<DataDefPTR *>(parameters[2]->datadef());
    return ptr_type ? ptr_type->base_type : NULL;
}

static std::string overflow_store_helper_suffix(DataDef *result_type)
{
    if ( !result_type || !result_type->is_integer() )
	return "s64";
    size_t size = result_type->size;
    if ( size <= 1 )
	return result_type->is_unsigned() ? "u8" : "s8";
    if ( size <= 2 )
	return result_type->is_unsigned() ? "u16" : "s16";
    if ( size <= 4 )
	return result_type->is_unsigned() ? "u32" : "s32";
    return result_type->is_unsigned() ? "u64" : "s64";
}

static DataType overflow_store_pointer_datatype(DataDef *result_type)
{
    if ( !result_type || !result_type->is_integer() )
	return rtPtr(DataType::dtINT64);
    return rtPtr(result_type->rawtype());
}

static datatype_vec_t overflow_store_helper_signature(DataDef *result_type)
{
    return datatype_vec_t{
	DataType::dtINT32,
	DataType::dtINT64,
	DataType::dtINT64,
	overflow_store_pointer_datatype(result_type)
    };
}

static bool overflow_inputs_are_unsigned64(const std::vector<TokenBase *> &parameters)
{
    if ( parameters.size() < 2 )
	return false;
    for ( int i = 0; i < 2; ++i )
    {
	if ( !parameters[i] )
	    return false;
	DataDef *dd = parameters[i]->datadef();
	if ( !dd || !dd->is_unsigned() || dd->size < 8 )
	    return false;
    }
    return true;
}

static std::string typed_overflow_store_symbol_name(const Variable &var,
						    const std::vector<TokenBase *> &parameters)
{
    if ( !is_overflow_store_helper_name(var.name) || parameters.size() < 3 )
	return runtime_symbol_name(var);
    DataDef *result_type = overflow_store_result_type(parameters);
    // When both inputs are unsigned 64-bit and the result is unsigned 64-bit,
    // use the unsigned-input helpers that reinterpret long long as unsigned
    // before widening to __int128.  This avoids sign-extension losing the
    // mathematical value (e.g. 0xFFFFFFFFFFFFFFEE → -18 instead of 2^64-18).
    if ( result_type && result_type->is_unsigned() && result_type->size >= 8
      && overflow_inputs_are_unsigned64(parameters) )
	return var.name + "_uu64";
    return var.name + "_" + overflow_store_helper_suffix(result_type);
}

static Operand &redirect_overflow_store_call(Program &pgm, const std::string &target_name,
					     DataDef *result_type,
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
		overflow_store_helper_signature(result_type),
		(fVOIDFUNC)sym);
	    if ( !target )
		target = pgm.findVariable(lookup);
	}
    }
    if ( !target || !target->type || !target->type->is_function() )
	pgm.Throw(site) << "missing overflow helper for " << site->var.name << flush;
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

static void emit_zero_fill_region(Program &pgm, x86::Gp &base_reg, int32_t base_ofs, size_t total)
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

static x86::Gp emit_runtime_struct_size(Program &pgm, DataDefSTRUCT *sdd, const char *name)
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

static bool is_large_struct_return(DataDef *ret_type)
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

static void emit_small_struct_return(Program &pgm, Operand &src, DataDef *ret_type)
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

static bool token_has_constant_cstring(TokenBase *token)
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

static int64_t estimate_constant_printf_output(TokenBase *fmt_token,
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

static bool type_is_cstr_pointer(DataDef *dd)
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

static void emit_zeroed_void_return(Program &pgm)
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

static void emit_function_instrument_exit(Program &pgm, FuncDef *current_func)
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
static x86::Xmm newScalarXmm(Program &pgm, DataDef *dd, const char *name)
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

static IRValue ir_from_operand(const Operand &op, DataDef *type)
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

static Operand &emit_ir_value(Program &pgm, IRValue value, regdefp_t &regdp,
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

static x86::Gp emit_bitfield_store_reg(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, x86::Gp value, const char *hint);
static x86::Gp emit_bitfield_store_operand(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, Operand &value, DataDef *value_type,
    DataDef *field_type, const char *hint);
static uint64_t bitfield_mask(size_t width);
static void emit_and_u64(Program &pgm, x86::Gp &gp, uint64_t mask, const char *hint);

static DataDef *infer_numeric_type(TokenBase *left, TokenBase *right);
static DataDef *complex_component_type(DataDef *dd);

static bool is_arithmetic_result_operator(TokenBase *token)
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

struct IntegerPrecision {
    bool valid;
    bool bitfield_derived;
    size_t bits;
    bool is_unsigned;

    IntegerPrecision()
	: valid(false), bitfield_derived(false), bits(0), is_unsigned(false) {}
    IntegerPrecision(size_t b, bool u, bool derived)
	: valid(true), bitfield_derived(derived), bits(b), is_unsigned(u) {}
};

static IntegerPrecision integer_precision_for_token(TokenBase *token);

static IntegerPrecision promoted_bitfield_precision(const DataDefSTRUCT::BitFieldInfo &bf)
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

static IntegerPrecision binary_integer_precision(TokenBase *left, TokenBase *right,
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

static DataDef *token_numeric_type(TokenBase *token)
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

static DataDef *choose_integer_type(TokenBase *left, DataDef *lt,
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

static DataDef *promote_c_integer_type(DataDef *type)
{
    if ( !type || !type->is_integer() )
	return type;
    if ( type->size < 4 )
	return &ddINT32;
    return type;
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

static DataDef *usual_binary_integer_type(TokenBase *left, DataDef *lt,
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

static DataDef *infer_numeric_type(TokenBase *left, TokenBase *right)
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

static DataDef *effective_pointer_type_for_arith(Program &pgm, TokenBase *tb);

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
static void splat_xmm_to_simd_xmm(Program &pgm, x86::Xmm &dst, x86::Xmm src,
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
static void splat_scalar_to_simd(Program &pgm, x86::Xmm &dst,
				 const IRValue &scalar, DataDefSIMD *vdd)
{
    if ( scalar.op.isReg() && scalar.op.as<BaseReg>().isGroup(RegGroup::kGp) )
	splat_gp_to_simd_xmm(pgm, dst, scalar.op.as<x86::Gp>(), vdd);
    else if ( scalar.op.isReg() && scalar.op.as<BaseReg>().isGroup(RegGroup::kVec) )
	splat_xmm_to_simd_xmm(pgm, dst, scalar.op.as<x86::Xmm>(), vdd);
    else
	throw "splat_scalar_to_simd() unexpected operand type";
}

static Operand &compile_token_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
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

static Operand &compile_compound_rhs_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
						Operand &storage)
{
    return compile_token_normalized(pgm, token, target_type, nullptr, storage);
}

static Operand &compile_token_gp_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
					    Operand &storage)
{
    Operand &out = compile_token_normalized(pgm, token, target_type, nullptr, storage);
    if ( !out.isReg() || !out.as<BaseReg>().isGroup(RegGroup::kGp) )
	throw "compile_token_gp_normalized() expected gp register";
    return out;
}

static Operand &compile_compound_rhs_gp_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
						   Operand &storage)
{
    return compile_token_gp_normalized(pgm, token, target_type, storage);
}

// Shared shape for the typesafe binary ops (safeadd / safesub /
// safemul): in-place on lval, producing lval := lval <op> rval. All
// three take a pair of DataDef slots (both default NULL) for
// per-operand type info; the emit_plain_binop3 helper only sets the
// left slot since both operands are pre-normalized to result_type.
typedef void (Program::*SafeBinOp3)(asmjit::Operand &, asmjit::Operand &, DataDef *, DataDef *);

// Shared shape for the 2-arg bitwise/shift typesafe ops (safeor /
// safeand / safexor / safeshl / safeshr): in-place on lval. No
// DataDef arg because these are type-agnostic (they operate on Gp
// registers regardless of integer width).
typedef void (Program::*SafeBitOp2)(asmjit::Operand &, asmjit::Operand &, DataDef *);

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

// Helper: load a variable's value into `dst_gp` with the size-aware move
// — mov from Gp, mov from Mem. Used by lambda-capture pack / multi-return
// unpack paths that need to move a var's value into a scratch Gp before
// storing it elsewhere.
static void load_var_to_gp(Program &pgm, Operand &src, asmjit::x86::Gp &dst_gp)
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
static void load_idx_to_gpq(Program &pgm, asmjit::x86::Gp &dst, asmjit::Operand &src)
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

static uint32_t scale_index_by_element_size(Program &pgm, asmjit::x86::Gp &idx_reg,
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
static void lea_var_to_gp(Program &pgm, Operand &src, asmjit::x86::Gp &dst_gp)
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
static void store_gp_to_var(Program &pgm, asmjit::x86::Gp &src_gp, Operand &dst)
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
struct GeneralBinopCascade {
    Operand *caller_dest;
    bool mirror_to_caller;
};

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
static bool is_fixed_array_struct_member(TokenBase *tb)
{
    TokenMember *tm = dynamic_cast<TokenMember *>(tb);
    return tm && tm->is_fixed_array_member();
}

static bool subscript_object_uses_inplace_storage(const Variable &object)
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

static DataDef *fixed_array_member_result_type(TokenMember *tm, size_t consumed_dims)
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

static DataDef *fixed_array_subscript_result_type(const Variable &object,
						  size_t consumed_dims)
{
    if ( !object.is_fixed_array() )
	return object.type ? object.type : &ddINT64;
    return build_fixed_array_result_type(object.type, object.dims, consumed_dims);
}

static size_t fixed_array_subscript_stride(const Variable &object,
					   size_t consumed_dims)
{
    DataDef *result_type = fixed_array_subscript_result_type(object, consumed_dims);
    return (result_type && result_type->size) ? result_type->size : 8;
}

static DataDef *effective_pointer_type_for_arith(Program &pgm, TokenBase *tb)
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

enum class CmpKind : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
enum class SimdBitKind : uint8_t { And, Or, Xor, Shl, Shr };

static Operand &compile_complex_compare_base(Program &pgm, TokenBase *expr, DataDef *expr_type,
					     Operand &storage)
{
    regdefp_t sub = {nullptr, expr_type, nullptr};
    Operand &base = expr->compile(pgm, sub);
    storage = base;
    return storage;
}

static x86::Mem complex_component_mem(Program &pgm, const Operand &base_op,
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

static bool is_large_simd_type(DataDef *type)
{
    return type && type->is_simd() && type->size > 16;
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

static x86::Mem simd_lane_mem(const x86::Mem &base, size_t lane, DataDefSIMD *vdd)
{
    DataDef *elem = vdd && vdd->element_type ? vdd->element_type : &ddINT64;
    size_t elem_size = elem->size ? elem->size : 8;
    x86::Mem out = base;
    out.addOffset((int64_t)(lane * elem_size));
    out.setSize((uint32_t)elem_size);
    return out;
}

static x86::Mem new_large_simd_stack(Program &pgm, DataDefSIMD *vdd, const char *name)
{
    (void)name;
    uint32_t bytes = (uint32_t)(vdd ? vdd->size : 16);
    uint32_t align = (uint32_t)(vdd ? vdd->alignment() : 16);
    x86::Mem mem = pgm.cc.newStack(bytes, align ? align : 16);
    mem.setSize(bytes);
    return mem;
}

static x86::Mem large_simd_expr_mem(Program &pgm, TokenBase *expr, DataDefSIMD *vdd,
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

static x86::Gp load_simd_lane_gp(Program &pgm, const x86::Mem &base, size_t lane,
				 DataDefSIMD *vdd, const char *name)
{
    DataDef *elem = vdd && vdd->element_type ? vdd->element_type : &ddINT64;
    x86::Gp gp = pgm.cc.newGpq("%s", name ? name : "simd_lane");
    x86::Mem mem = simd_lane_mem(base, lane, vdd);
    load_mem_to_gpq(pgm, gp, mem, elem);
    return gp;
}

static void store_simd_lane_gp(Program &pgm, const x86::Mem &base, size_t lane,
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

static void prepare_complex_component_pair(Program &pgm, TokenBase *expr, DataDef *target_complex_type,
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

static DataDef *builtin_complex_compat_type(DataDef *base_type)
{
    static std::map<DataDef *, DataDefCOMPLEX *> cache;
    if ( !base_type )
	return &ddVOID;
    std::map<DataDef *, DataDefCOMPLEX *>::iterator it = cache.find(base_type);
    if ( it != cache.end() )
	return it->second;
    DataDefCOMPLEX *complex_type = new DataDefCOMPLEX(*base_type);
    cache[base_type] = complex_type;
    return complex_type;
}

static DataDef *builtin_complex_arg_type(TokenCallFunc *call)
{
    if ( !call )
	return NULL;
    DataDef *ret_type = call->returns();
    if ( ret_type && ret_type->is_complex() )
	return ret_type;
    if ( ret_type && ret_type->is_real() )
	return builtin_complex_compat_type(ret_type);
    return call->argc() == 1 && call->parameters[0] ? call->parameters[0]->datadef() : NULL;
}

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

static Operand &compile_call_arg_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
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

static Operand &emit_complex_from_scalar(Program &pgm, Operand &scalar_op, DataDef *scalar_type,
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

static Operand &emit_complex_conjugate_expr(Program &pgm, TokenBase *arg_token,
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

static Operand &emit_builtin_complex_conjugate(Program &pgm, TokenCallFunc *call,
					       regdefp_t &regdp, Operand &storage)
{
    if ( !call || call->argc() != 1 )
	throw "emit_builtin_complex_conjugate() expects one argument";

    TokenBase *arg_token = call->parameters[0];
    DataDef *complex_type = builtin_complex_arg_type(call);
    if ( !complex_type || !complex_type->is_complex() )
	throw "emit_builtin_complex_conjugate() expects complex argument";
    return emit_complex_conjugate_expr(pgm, arg_token, complex_type, regdp, storage);
}

static Operand &emit_builtin_complex_component(Program &pgm, TokenCallFunc *call,
					       bool imag_part,
					       regdefp_t &regdp, Operand &storage)
{
    if ( !call || call->argc() != 1 )
	throw "emit_builtin_complex_component() expects one argument";

    TokenBase *arg_token = call->parameters[0];
    DataDef *complex_type = builtin_complex_arg_type(call);
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_type);
    if ( !cdd || !cdd->element_type )
	throw "emit_builtin_complex_component() expects complex argument";

    Operand base_storage;
    Operand real_storage;
    Operand imag_storage;
    Operand *real_part = NULL;
    Operand *imag_part_op = NULL;
    DataDef *real_type = NULL;
    DataDef *imag_type = NULL;
    prepare_complex_component_pair(pgm, arg_token, complex_type, base_storage,
				   real_part, real_type, imag_part_op, imag_type,
				   real_storage, imag_storage);
    Operand &part = imag_part ? *imag_part_op : *real_part;
    DataDef *part_type = imag_part ? imag_type : real_type;
    return emit_ir_value(pgm, ir_from_operand(part, part_type), regdp, storage,
			 cdd->element_type);
}

static void add_funcsig_arg(FuncSignature &funcsig, DataDef *arg_type)
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

static void append_call_param(std::vector<Operand> &params, FuncSignature &funcsig,
			      const Operand &arg, DataDef *arg_type)
{
    params.push_back(arg);
    add_funcsig_arg(funcsig, arg_type);
}

static void set_invoke_arg(Program &pgm, InvokeNode *call, uint32_t &index,
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

static void set_invoke_args(Program &pgm, InvokeNode *call, const std::vector<Operand> &params,
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

static void *resolve_setjmp_target()
{
    void *target = dlsym(RTLD_DEFAULT, "_setjmp");
    if ( !target )
	target = dlsym(RTLD_DEFAULT, "setjmp");
    return target;
}

static Operand &emit_builtin_setjmp(Program &pgm, TokenCallFunc *call,
				    regdefp_t &regdp, Operand &fallback_operand)
{
    if ( call->argc() != 1 )
	pgm.Throw(call) << "__builtin_setjmp expects one argument" << flush;

    DataDef *buf_type = NULL;
    Operand buf_storage;
    Operand &buf_arg = compile_call_arg_normalized(pgm, call->parameters[0],
	&ddVOIDptr, false, buf_storage, buf_type);

    FuncSignature env_sig(CallConvId::kCDecl);
    env_sig.setRetT<void *>();
    std::vector<Operand> env_params;
    append_call_param(env_params, env_sig, buf_arg, &ddVOIDptr);

    void *env_target = (void *)(intptr_t)__madc_jmpbuf_for;
    InvokeNode *env_call;
    pgm.cc.invoke(&env_call, imm(env_target), env_sig);
    pgm.track_invoke_target(env_target);
    set_invoke_args(pgm, env_call, env_params, false);

    x86::Gp env_ptr = pgm.cc.newIntPtr("__builtin_setjmp_env");
    env_call->setRet(0, env_ptr);

    void *setjmp_target = resolve_setjmp_target();
    if ( !setjmp_target )
	pgm.Throw(call) << "__builtin_setjmp could not resolve _setjmp" << flush;

    FuncSignature setjmp_sig(CallConvId::kCDecl);
    setjmp_sig.setRetT<int>();
    std::vector<Operand> setjmp_params;
    append_call_param(setjmp_params, setjmp_sig, env_ptr, &ddVOIDptr);

    InvokeNode *setjmp_call;
    pgm.cc.invoke(&setjmp_call, imm(setjmp_target), setjmp_sig);
    pgm.track_invoke_target(setjmp_target);
    set_invoke_args(pgm, setjmp_call, setjmp_params, false);

    if ( !regdp.first )
    {
	fallback_operand = pgm.cc.newGpq("__builtin_setjmp_ret");
	regdp.first = &fallback_operand;
    }
    bind_call_return(pgm, setjmp_call, regdp.first, &ddINT32,
		     fallback_operand, false, true);
    regdp.second = &ddINT32;
    return *regdp.first;
}

static Operand &emit_builtin_longjmp(Program &pgm, TokenCallFunc *call,
				     regdefp_t &regdp, Operand &fallback_operand)
{
    if ( call->argc() != 2 )
	pgm.Throw(call) << "__builtin_longjmp expects two arguments" << flush;

    FuncSignature funcsig(CallConvId::kCDecl);
    funcsig.setRetT<void>();
    std::vector<Operand> params;

    DataDef *buf_type = NULL;
    Operand buf_storage;
    Operand &buf_arg = compile_call_arg_normalized(pgm, call->parameters[0],
	&ddVOIDptr, false, buf_storage, buf_type);
    append_call_param(params, funcsig, buf_arg, &ddVOIDptr);

    DataDef *value_type = NULL;
    Operand value_storage;
    Operand &value_arg = compile_call_arg_normalized(pgm, call->parameters[1],
	&ddINT32, false, value_storage, value_type);
    append_call_param(params, funcsig, value_arg, &ddINT32);

    void *target = (void *)(intptr_t)__madc_builtin_longjmp;
    InvokeNode *invoke;
    pgm.cc.invoke(&invoke, imm(target), funcsig);
    pgm.track_invoke_target(target);
    set_invoke_args(pgm, invoke, params, false);

    regdp.second = &ddVOID;
    fallback_operand = imm(0);
    return fallback_operand;
}

// __builtin_alloca(size) — allocate on the stack via sub rsp.
// asmjit Compiler mode uses rbp-based frames, so the epilogue
// (leave = mov rsp,rbp; pop rbp; ret) undoes the allocation.
// GCC emits: align size to 16, sub rsp, return rsp.
static Operand &emit_builtin_alloca(Program &pgm, TokenCallFunc *call,
				    regdefp_t &regdp, Operand &fallback_operand)
{
    if ( call->argc() != 1 )
	pgm.Throw(call) << "__builtin_alloca expects one argument" << flush;

    DBG(pgm.cc.comment("__builtin_alloca"));

    // Compile size argument
    regdefp_t size_rdp = {NULL, &ddINT64, NULL};
    Operand &size_op = call->parameters[0]->compile(pgm, size_rdp);
    x86::Gp size_gp = pgm.cc.newGpq("alloca_size");
    if ( size_op.isReg() && size_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	pgm.cc.mov(size_gp, size_op.as<x86::Gp>());
    else if ( size_op.isMem() )
	pgm.cc.mov(size_gp, size_op.as<x86::Mem>());
    else if ( size_op.isImm() )
	pgm.cc.mov(size_gp, size_op.as<Imm>());
    else
	pgm.Throw(call) << "__builtin_alloca: unsupported size operand" << flush;

    // Align to 16 bytes: size = (size + 15) & ~15
    pgm.cc.add(size_gp, imm(15));
    pgm.cc.and_(size_gp, imm(~(int64_t)15));

    // sub rsp, size
    pgm.cc.sub(x86::rsp, size_gp);

    // result = rsp (the newly allocated region)
    x86::Gp result = pgm.cc.newIntPtr("alloca_ptr");
    pgm.cc.mov(result, x86::rsp);

    DataDef *ret_type = pgm.getPointerType(&ddVOID);
    if ( !regdp.second )
	regdp.second = ret_type;
    return emit_ir_value(pgm, IRValue::reg(result, ret_type), regdp, fallback_operand, ret_type);
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

static Operand &compile_complex_condition_operand(Program &pgm, TokenBase *condition,
						  DataDefCOMPLEX *cdd, Operand &storage)
{
    DataDef *component_type = cdd && cdd->element_type ? cdd->element_type : &ddDOUBLE;
    TokenComplexPart real_part(condition, false);
    TokenComplexPart imag_part(condition, true);
    Operand real_storage;
    Operand imag_storage;
    Operand &real_op = compile_token_normalized(pgm, &real_part, component_type, nullptr, real_storage);
    Operand &imag_op = compile_token_normalized(pgm, &imag_part, component_type, nullptr, imag_storage);

    pgm.testzero(real_op);
    x86::Gp real_nonzero = pgm.cc.newGpq("__complex_cond_re");
    pgm.safesetne(real_nonzero);

    pgm.testzero(imag_op);
    x86::Gp imag_nonzero = pgm.cc.newGpq("__complex_cond_im");
    pgm.safesetne(imag_nonzero);

    pgm.safeor(real_nonzero, imag_nonzero, &ddINT64);
    storage = real_nonzero;
    return storage;
}

static Operand &compile_condition_operand(Program &pgm, TokenBase *condition, Operand &storage)
{
    regdefp_t condrdp = {NULL, NULL, NULL};
    Operand &raw = condition->compile(pgm, condrdp);
    DataDef *cond_type = condrdp.second
	? condrdp.second
	: (condition && condition->datadef() ? condition->datadef() : &ddINT64);

    if ( DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(cond_type) )
	return compile_complex_condition_operand(pgm, condition, cdd, storage);

    // Preserve literal conditions as immediates so TokenIF/WHILE/FOR can
    // short-circuit dead branches like `if (0) ...` before compiling them.
    if ( raw.isImm() )
    {
	storage = raw;
	return storage;
    }

    if ( cond_type && (cond_type->is_numeric() || cond_type->is_pointer())
      && (raw.isReg() || raw.isMem()) )
    {
	IRBuilder ir(pgm.cc);
	IRValue cond_value = ir.load(ir.coerce(ir_from_operand(raw, cond_type), cond_type));
	storage = cond_value.op;
	return storage;
    }

    storage = raw;
    return storage;
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

void streamout_string(std::ostream &os, std::string &s)
{
//  DBG(std::cout << "streamout_string: << " << (uint64_t)&s << std::endl);
    os << s;
}

void streamout_cstr(std::ostream &os, const char *s)
{
    os << (s ? s : "(null)");
}

void streamout_int(std::ostream &os, int i)
{
//  DBG(std::cout << "streamout_int: << " << i << std::endl);
    os << i;
}

template<typename T> void streamout_numeric(std::ostream &os, T i)
{
//  DBG(std::cout << "streamout_numeric: sizeof(i) " << sizeof(i) << std::endl);
    os << i;
}

void streamout_intptr(std::ostream &os, int *i)
{
    if ( !i ) { std::cerr << "ERROR: streamout_intptr: NULL!" << std::endl; return; }
    DBG(std::cout << "streamout_intptr: << " << *i << std::endl);
    os << *i;
}


// stream input helpers for double (streamin_string/streamin_int already exist above)
void *streamin_double(void *stream, void *val)
{
    *(std::istream *)stream >> *(double *)val;
    return stream;
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

    if ( const char *target_name = simple_libc_builtin_redirect_target(var.name) )
	return redirect_builtin_call(pgm, target_name, parameters, regdp, this, _operand);
    if ( is_overflow_predicate_helper_name(var.name) )
    {
	std::string target_name = typed_overflow_predicate_symbol_name(var, parameters);
	return redirect_overflow_predicate_call(pgm, target_name, parameters, regdp, this, _operand);
    }
    if ( is_overflow_store_helper_name(var.name) )
    {
	DataDef *result_type = overflow_store_result_type(parameters);
	std::string target_name = typed_overflow_store_symbol_name(var, parameters);
	return redirect_overflow_store_call(pgm, target_name, result_type, parameters, regdp, this, _operand);
    }

    if ( argc() == 2 && var.name == "__builtin_object_size" )
    {
	int64_t mode = 0;
	try_eval_const_i64(parameters[1], mode);
	int64_t size = estimate_object_size(parameters[0], (int)mode);
	regdp.second = &ddUINT64;
	return emit_ir_value(pgm,
	    IRValue::imm(Imm(size >= 0 ? size : -1), &ddUINT64),
	    regdp, _operand, &ddUINT64);
    }

    // __builtin_shuffle(vec, mask) — element-wise lane permutation.
    // result[i] = vec[mask[i] % lane_count]
    if ( (argc() == 2 || argc() == 3) && var.name == "__builtin_shuffle" )
    {
	// Resolve the source vector SIMD type
	DataDef *src_dd = parameters[0]->datadef();
	DataDefSIMD *vdd = src_dd ? dynamic_cast<DataDefSIMD *>(src_dd) : nullptr;
	if ( !vdd )
	    pgm.Throw(this) << "__builtin_shuffle: first argument must be a SIMD vector" << flush;

	size_t lanes = vdd->lane_count;
	size_t elem_size = vdd->element_type ? vdd->element_type->size : 8;
	DataDef *elem_type = vdd->element_type ? vdd->element_type : &ddINT64;
	bool is_float_elem = elem_type->is_real();

	// Allocate stack slots for source, mask, and result
	x86::Mem src_slot = new_large_simd_stack(pgm, vdd, "shuf_src_slot");
	x86::Mem result_mem = new_large_simd_stack(pgm, vdd, "shuf_result");

	// Compile source and store to our stack slot
	{
	    regdefp_t src_rdp = {nullptr, vdd, nullptr};
	    Operand &src_op = parameters[0]->compile(pgm, src_rdp);
	    if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kVec) )
		store_xmm_to_mem(pgm, src_slot, src_op.as<x86::Xmm>(), vdd);
	    else if ( src_op.isMem() )
	    {
		// Copy from source Mem to our stack slot
		x86::Gp copy_base_src = pgm.cc.newIntPtr("shuf_copy_src");
		x86::Gp copy_base_dst = pgm.cc.newIntPtr("shuf_copy_dst");
		pgm.cc.lea(copy_base_src, src_op.as<x86::Mem>());
		pgm.cc.lea(copy_base_dst, src_slot);
		for ( size_t b = 0; b < vdd->size; b += 8 )
		{
		    x86::Gp tmp = pgm.cc.newGpq("shuf_cp");
		    pgm.cc.mov(tmp, x86::ptr(copy_base_src, (int32_t)b, 8));
		    pgm.cc.mov(x86::ptr(copy_base_dst, (int32_t)b, 8), tmp);
		}
	    }
	    else
		pgm.Throw(this) << "__builtin_shuffle: unsupported source operand shape" << flush;
	}

	// For 2-source shuffle (argc==3), spill second source too
	x86::Mem src2_slot;
	bool two_source = (argc() == 3);
	if ( two_source )
	{
	    src2_slot = new_large_simd_stack(pgm, vdd, "shuf_src2_slot");
	    regdefp_t src2_rdp = {nullptr, vdd, nullptr};
	    Operand &src2_op = parameters[1]->compile(pgm, src2_rdp);
	    if ( src2_op.isReg() && src2_op.as<BaseReg>().isGroup(RegGroup::kVec) )
		store_xmm_to_mem(pgm, src2_slot, src2_op.as<x86::Xmm>(), vdd);
	    else if ( src2_op.isMem() )
	    {
		x86::Gp cs = pgm.cc.newIntPtr("shuf_cp2s");
		x86::Gp cd = pgm.cc.newIntPtr("shuf_cp2d");
		pgm.cc.lea(cs, src2_op.as<x86::Mem>());
		pgm.cc.lea(cd, src2_slot);
		for ( size_t b = 0; b < vdd->size; b += 8 )
		{
		    x86::Gp tmp = pgm.cc.newGpq("shuf_cp2");
		    pgm.cc.mov(tmp, x86::ptr(cs, (int32_t)b, 8));
		    pgm.cc.mov(x86::ptr(cd, (int32_t)b, 8), tmp);
		}
	    }
	}

	// Compile mask and store to stack slot
	TokenBase *mask_param = parameters[argc() - 1];
	DataDef *mask_dd = mask_param->datadef();
	DataDefSIMD *mask_vdd = mask_dd ? dynamic_cast<DataDefSIMD *>(mask_dd) : nullptr;
	if ( !mask_vdd )
	    pgm.Throw(this) << "__builtin_shuffle: mask must be a SIMD vector" << flush;
	x86::Mem mask_slot = new_large_simd_stack(pgm, mask_vdd, "shuf_mask_slot");
	{
	    regdefp_t mask_rdp = {nullptr, mask_vdd, nullptr};
	    Operand &mask_op = mask_param->compile(pgm, mask_rdp);
	    if ( mask_op.isReg() && mask_op.as<BaseReg>().isGroup(RegGroup::kVec) )
		store_xmm_to_mem(pgm, mask_slot, mask_op.as<x86::Xmm>(), mask_vdd);
	    else if ( mask_op.isMem() )
	    {
		x86::Gp cs = pgm.cc.newIntPtr("shuf_cpm_s");
		x86::Gp cd = pgm.cc.newIntPtr("shuf_cpm_d");
		pgm.cc.lea(cs, mask_op.as<x86::Mem>());
		pgm.cc.lea(cd, mask_slot);
		for ( size_t b = 0; b < mask_vdd->size; b += 8 )
		{
		    x86::Gp tmp = pgm.cc.newGpq("shuf_cpm");
		    pgm.cc.mov(tmp, x86::ptr(cs, (int32_t)b, 8));
		    pgm.cc.mov(x86::ptr(cd, (int32_t)b, 8), tmp);
		}
	    }
	}

	x86::Gp src_base = pgm.cc.newIntPtr("shuf_src_base");
	pgm.cc.lea(src_base, src_slot);

	x86::Gp src2_base;
	if ( two_source )
	{
	    src2_base = pgm.cc.newIntPtr("shuf_src2_base");
	    pgm.cc.lea(src2_base, src2_slot);
	}

	size_t mask_mod = two_source ? (lanes * 2) : lanes;

	// For each output lane: load mask index, compute source offset, copy element
	for ( size_t i = 0; i < lanes; ++i )
	{
	    // Load mask[i] as integer
	    x86::Gp idx = load_simd_lane_gp(pgm, mask_slot, i, mask_vdd, "shuf_idx");

	    // idx = idx % mask_mod (mask_mod is always power of 2)
	    pgm.cc.and_(idx, imm((int64_t)(mask_mod - 1)));

	    // Compute byte offset: idx * elem_size
	    if ( elem_size > 1 )
	    {
		x86::Gp shift_tmp = pgm.cc.newGpq("shuf_shift");
		pgm.cc.mov(shift_tmp, idx);
		if ( elem_size == 2 ) pgm.cc.shl(shift_tmp, 1);
		else if ( elem_size == 4 ) pgm.cc.shl(shift_tmp, 2);
		else if ( elem_size == 8 ) pgm.cc.shl(shift_tmp, 3);
		else
		{
		    pgm.cc.imul(shift_tmp, imm((int64_t)elem_size));
		}
		idx = shift_tmp;
	    }

	    // For 2-source: if original index >= lanes, use src2_base
	    x86::Gp base_ptr;
	    if ( two_source )
	    {
		// Reload unmasked index for source selection
		x86::Gp raw_idx = load_simd_lane_gp(pgm, mask_slot, i, mask_vdd, "shuf_raw_idx");
		pgm.cc.and_(raw_idx, imm((int64_t)(mask_mod - 1)));

		base_ptr = pgm.cc.newIntPtr("shuf_base_sel");
		pgm.cc.mov(base_ptr, src_base);
		pgm.cc.cmp(raw_idx, imm((int64_t)lanes));
		Label use_src1 = pgm.cc.newLabel();
		pgm.cc.jb(use_src1);
		pgm.cc.mov(base_ptr, src2_base);
		// Adjust offset: subtract lanes * elem_size
		pgm.cc.sub(idx, imm((int64_t)(lanes * elem_size)));
		pgm.cc.bind(use_src1);
	    }
	    else
		base_ptr = src_base;

	    // Compute final element address: base + byte_offset
	    x86::Gp elem_addr = pgm.cc.newIntPtr("shuf_elem_addr");
	    pgm.cc.mov(elem_addr, base_ptr);
	    pgm.cc.add(elem_addr, idx);
	    x86::Mem src_elem = x86::ptr(elem_addr, 0, (uint32_t)elem_size);
	    x86::Mem dst_lane = simd_lane_mem(result_mem, i, vdd);

	    if ( is_float_elem && elem_size == 4 )
	    {
		x86::Xmm tmp = pgm.cc.newXmm("shuf_ftmp");
		pgm.cc.movss(tmp, src_elem);
		pgm.cc.movss(dst_lane, tmp);
	    }
	    else if ( is_float_elem && elem_size == 8 )
	    {
		x86::Xmm tmp = pgm.cc.newXmm("shuf_dtmp");
		pgm.cc.movsd(tmp, src_elem);
		pgm.cc.movsd(dst_lane, tmp);
	    }
	    else
	    {
		x86::Gp tmp = pgm.cc.newGpq("shuf_itmp");
		load_mem_to_gpq(pgm, tmp, src_elem, elem_type);
		store_simd_lane_gp(pgm, result_mem, i, vdd, tmp);
	    }
	}

	// Store result into caller's destination if provided, else new Xmm
	regdp.second = vdd;
	if ( regdp.first && regdp.first->isReg()
	  && regdp.first->as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    // Caller provided an Xmm destination — load result directly into it
	    x86::Xmm dst_xmm = regdp.first->as<x86::Xmm>();
	    if ( vdd->size == 8 )
		pgm.cc.movq(dst_xmm, result_mem);
	    else
		pgm.cc.movups(dst_xmm, result_mem);
	}
	else if ( regdp.first && regdp.first->isMem() )
	{
	    // Caller provided a Mem destination — copy result bytes
	    x86::Gp rs = pgm.cc.newIntPtr("shuf_res_src");
	    x86::Gp rd = pgm.cc.newIntPtr("shuf_res_dst");
	    pgm.cc.lea(rs, result_mem);
	    pgm.cc.lea(rd, regdp.first->as<x86::Mem>());
	    for ( size_t b = 0; b < vdd->size; b += 8 )
	    {
		x86::Gp t = pgm.cc.newGpq("shuf_rcp");
		pgm.cc.mov(t, x86::ptr(rs, (int32_t)b, 8));
		pgm.cc.mov(x86::ptr(rd, (int32_t)b, 8), t);
	    }
	}
	else
	{
	    // No caller destination — create a new Xmm or return Mem
	    if ( vdd->size <= 16 )
	    {
		x86::Xmm result_xmm = pgm.cc.newXmm("shuf_result");
		if ( vdd->size == 8 )
		    pgm.cc.movq(result_xmm, result_mem);
		else
		    pgm.cc.movups(result_xmm, result_mem);
		_operand = result_xmm;
	    }
	    else
		_operand = result_mem;
	    regdp.first = &_operand;
	}
	return _operand;
    }

    if ( argc() == 6 && var.name == "__builtin___vsnprintf_chk" )
    {
	int64_t flag = 0;
	int64_t len = -1;
	int64_t size = -1;
	bool flag_const = try_eval_const_i64(parameters[2], flag);
	bool len_const = try_eval_const_i64(parameters[1], len);
	bool size_const = try_eval_const_i64(parameters[3], size);
	bool use_plain = flag_const && flag == 0
	    && ((size_const && size < 0)
	     || (len_const && size_const && len >= 0 && size >= 0
	      && (uint64_t)len <= (uint64_t)size));

	std::string target_name = use_plain ? "vsnprintf" : "__vsnprintf_chk";
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	redirected_args.push_back(parameters[1]);
	if ( use_plain )
	{
	    redirected_args.push_back(parameters[4]);
	    redirected_args.push_back(parameters[5]);
	}
	else
	{
	    redirected_args.push_back(parameters[2]);
	    redirected_args.push_back(parameters[3]);
	    redirected_args.push_back(parameters[4]);
	    redirected_args.push_back(parameters[5]);
	}
	return redirect_builtin_call(pgm, target_name, redirected_args, regdp, this, _operand);
    }

    if ( argc() == 5 && var.name == "__builtin___vsprintf_chk" )
    {
	int64_t flag = 0;
	int64_t size = -1;
	bool flag_const = try_eval_const_i64(parameters[1], flag);
	bool size_const = try_eval_const_i64(parameters[2], size);
	int64_t est_len = estimate_constant_printf_output(parameters[3], parameters, 4);
	bool use_plain = flag_const && flag == 0
	    && ((size_const && size < 0)
	     || (size_const && est_len >= 0 && size >= 0
	      && (uint64_t)est_len < (uint64_t)size));

	std::string target_name = use_plain ? "vsprintf" : "__vsprintf_chk";
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	if ( use_plain )
	{
	    redirected_args.push_back(parameters[3]);
	    redirected_args.push_back(parameters[4]);
	}
	else
	{
	    redirected_args.push_back(parameters[1]);
	    redirected_args.push_back(parameters[2]);
	    redirected_args.push_back(parameters[3]);
	    redirected_args.push_back(parameters[4]);
	}
	return redirect_builtin_call(pgm, target_name, redirected_args, regdp, this, _operand);
    }

    if ( argc() >= 4 && var.name == "__builtin___sprintf_chk" )
    {
	int64_t flag = 0;
	int64_t size = -1;
	bool flag_const = try_eval_const_i64(parameters[1], flag);
	bool size_const = try_eval_const_i64(parameters[2], size);
	int64_t est_len = estimate_constant_printf_output(parameters[3], parameters, 4);
	bool use_plain = flag_const && flag == 0
	    && ((size_const && size < 0)
	     || (size_const && est_len >= 0 && size >= 0
	      && (uint64_t)est_len < (uint64_t)size));

	std::string target_name = use_plain ? "sprintf" : "__sprintf_chk";
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	if ( use_plain )
	{
	    for ( size_t i = 3; i < parameters.size(); ++i )
		redirected_args.push_back(parameters[i]);
	}
	else
	{
	    redirected_args.push_back(parameters[1]);
	    redirected_args.push_back(parameters[2]);
	    for ( size_t i = 3; i < parameters.size(); ++i )
		redirected_args.push_back(parameters[i]);
	}
	return redirect_builtin_call(pgm, target_name, redirected_args, regdp, this, _operand);
    }

    if ( argc() >= 5 && var.name == "__builtin___snprintf_chk" )
    {
	int64_t flag = 0;
	int64_t len = -1;
	int64_t size = -1;
	bool flag_const = try_eval_const_i64(parameters[2], flag);
	bool len_const = try_eval_const_i64(parameters[1], len);
	bool size_const = try_eval_const_i64(parameters[3], size);
	bool use_plain = flag_const && flag == 0
	    && ((size_const && size < 0)
	     || (len_const && size_const && len >= 0 && size >= 0
	      && (uint64_t)len <= (uint64_t)size));

	std::string target_name = use_plain ? "snprintf" : "__snprintf_chk";
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	redirected_args.push_back(parameters[1]);
	if ( use_plain )
	{
	    for ( size_t i = 4; i < parameters.size(); ++i )
		redirected_args.push_back(parameters[i]);
	}
	else
	{
	    redirected_args.push_back(parameters[2]);
	    redirected_args.push_back(parameters[3]);
	    for ( size_t i = 4; i < parameters.size(); ++i )
		redirected_args.push_back(parameters[i]);
	}
	return redirect_builtin_call(pgm, target_name, redirected_args, regdp, this, _operand);
    }

    if ( argc() == 4 && (var.name == "__builtin___memset_chk"
		      || var.name == "__builtin___memcpy_chk"
		      || var.name == "__builtin___memmove_chk"
		      || var.name == "__builtin___mempcpy_chk") )
    {
	int64_t len = -1;
	int64_t size = -1;
	bool len_const = try_eval_const_i64(parameters[2], len);
	bool size_const = try_eval_const_i64(parameters[3], size);
	bool use_plain = (size_const && size < 0)
	    || (len_const && size_const && len >= 0 && size >= 0
	     && (uint64_t)len <= (uint64_t)size);
	std::string target_name;
	if ( var.name == "__builtin___memset_chk" )
	    target_name = use_plain ? "memset" : "__memset_chk";
	else if ( var.name == "__builtin___memcpy_chk" )
	    target_name = use_plain ? "memcpy" : "__memcpy_chk";
	else if ( var.name == "__builtin___memmove_chk" )
	    target_name = use_plain ? "memmove" : "__memmove_chk";
	else
	    target_name = use_plain ? "mempcpy" : "__mempcpy_chk";
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	redirected_args.push_back(parameters[1]);
	redirected_args.push_back(parameters[2]);
	if ( !use_plain )
	    redirected_args.push_back(parameters[3]);
	return redirect_builtin_call(pgm, target_name, redirected_args, regdp, this, _operand);
    }

    if ( argc() == 4 && (var.name == "__builtin___strncpy_chk"
		      || var.name == "__builtin___strncat_chk") )
    {
	int64_t len = -1;
	int64_t size = -1;
	bool len_const = try_eval_const_i64(parameters[2], len);
	bool size_const = try_eval_const_i64(parameters[3], size);
	bool use_plain = (size_const && size < 0)
	    || (len_const && size_const && len >= 0 && size >= 0
	     && (uint64_t)len <= (uint64_t)size);
	std::string target_name;
	if ( var.name == "__builtin___strncpy_chk" )
	    target_name = use_plain ? "strncpy" : "__strncpy_chk";
	else
	    target_name = use_plain ? "strncat" : "__strncat_chk";
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	redirected_args.push_back(parameters[1]);
	redirected_args.push_back(parameters[2]);
	if ( !use_plain )
	    redirected_args.push_back(parameters[3]);
	return redirect_builtin_call(pgm, target_name, redirected_args, regdp, this, _operand);
    }

    if ( argc() == 3 && (var.name == "__builtin___strcpy_chk"
		      || var.name == "__builtin___stpcpy_chk"
		      || var.name == "__builtin___strcat_chk") )
    {
	int64_t size = -1;
	bool size_const = try_eval_const_i64(parameters[2], size);
	bool use_plain = size_const && size < 0;
	std::string target_name;
	if ( var.name == "__builtin___strcpy_chk" )
	    target_name = use_plain ? "strcpy" : "__strcpy_chk";
	else if ( var.name == "__builtin___stpcpy_chk" )
	    target_name = use_plain ? "stpcpy" : "__stpcpy_chk";
	else
	    target_name = use_plain ? "strcat" : "__strcat_chk";
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	redirected_args.push_back(parameters[1]);
	if ( !use_plain )
	    redirected_args.push_back(parameters[2]);
	return redirect_builtin_call(pgm, target_name, redirected_args, regdp, this, _operand);
    }

    if ( argc() == 4 && var.name == "__builtin___stpncpy_chk" )
    {
	int64_t len = -1;
	int64_t size = -1;
	bool len_const = try_eval_const_i64(parameters[2], len);
	bool size_const = try_eval_const_i64(parameters[3], size);
	bool use_plain = (size_const && size < 0)
	    || (len_const && size_const && len >= 0 && size >= 0
	     && (uint64_t)len <= (uint64_t)size);
	std::vector<TokenBase *> redirected_args;
	redirected_args.push_back(parameters[0]);
	redirected_args.push_back(parameters[1]);
	redirected_args.push_back(parameters[2]);
	if ( use_plain )
	    return redirect_builtin_call(pgm, "stpncpy", redirected_args, regdp, this, _operand);
	redirected_args.push_back(parameters[3]);
	return redirect_builtin_call(pgm, "__stpncpy_chk", redirected_args, regdp, this, _operand);
    }

    if ( argc() == 1 && (var.name == "__builtin_conj"
		      || var.name == "__builtin_conjf"
		      || var.name == "__builtin_conjl"
		      || var.name == "conj"
		      || var.name == "conjf"
		      || var.name == "conjl") )
	return emit_builtin_complex_conjugate(pgm, this, regdp, _operand);
    if ( argc() == 1 && (var.name == "__builtin_creal"
		      || var.name == "__builtin_crealf"
		      || var.name == "__builtin_creall"
		      || var.name == "creal"
		      || var.name == "crealf"
		      || var.name == "creall") )
	return emit_builtin_complex_component(pgm, this, false, regdp, _operand);
    if ( argc() == 1 && (var.name == "__builtin_cimag"
		      || var.name == "__builtin_cimagf"
		      || var.name == "__builtin_cimagl"
		      || var.name == "cimag"
		      || var.name == "cimagf"
		      || var.name == "cimagl") )
	return emit_builtin_complex_component(pgm, this, true, regdp, _operand);
    if ( argc() == 1 && var.name == "__builtin_frame_address" )
    {
	DataDef *ret_type = returns();
	regdp.second = ret_type;
	x86::Gp frame_gp = pgm.cc.newIntPtr("__builtin_frame_address");
	pgm.cc.mov(frame_gp, x86::rsp);
	return emit_ir_value(pgm, IRValue::reg(frame_gp, ret_type), regdp, _operand, ret_type);
    }
    if ( var.name == "__builtin_setjmp" )
	return emit_builtin_setjmp(pgm, this, regdp, _operand);
    if ( var.name == "__builtin_longjmp" )
	return emit_builtin_longjmp(pgm, this, regdp, _operand);
    // __builtin_alloca: currently mapped to malloc by the lexer.
    // Real sub-rsp alloca needs asmjit PreservedFP (rbp-based frames)
    // which breaks other tests.  Left as future work.

    // GCC builtin parity: direct abs-family calls keep builtin semantics
    // unless the specific plain-name builtin is disabled (e.g.
    // -fno-builtin-abs). __builtin_* forms always keep builtin behavior.
    if ( argc() == 1
      && is_direct_abs_builtin_name(var.name)
      && !direct_abs_builtin_disabled(pgm, var.name) )
    {
	DataDef *builtin_type = direct_abs_builtin_type(var.name);
	FuncSignature funcsig(CallConvId::kCDecl);
	if ( direct_abs_builtin_returns_32bit(var.name) )
	    funcsig.setRetT<int>();
	else
	    funcsig.setRetT<int64_t>();
	add_funcsig_arg(funcsig, builtin_type);

	Operand arg_storage;
	DataDef *arg_type = NULL;
	Operand &arg = compile_call_arg_normalized(pgm, parameters[0], builtin_type, true, arg_storage, arg_type);
	std::vector<Operand> params;
	append_call_param(params, funcsig, arg, builtin_type);

	void *builtin_target = direct_abs_builtin_target(var.name);

	InvokeNode *call;
	pgm.cc.invoke(&call, imm(builtin_target), funcsig);
	pgm.track_invoke_target(builtin_target);
	set_invoke_args(pgm, call, params, false);

	if ( !regdp.first )
	{
	    _operand = pgm.cc.newGpq("builtin_abs_ret");
	    regdp.first = &_operand;
	}
	bind_call_return(pgm, call, regdp.first, builtin_type, _operand,
			 /*is_variadic=*/false, false);
	regdp.second = builtin_type;
	return *regdp.first;
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
	Operand &tnreg = compile_call_arg_normalized(pgm, tn, ptype, is_variadic, arg_storage, arg_type);
	validate_call_arg_type(pgm, tn, ptype, arg_type, tnreg);
	DBG(pgm.cc.comment("TokenCallFunc::compile() params.push_back(tnreg)"));
	// could probably use a tv->var.addArgT(funcsig) method
	DBG(pgm.cc.comment(ptype->name.c_str() /*arg_type->name.c_str()*/));
	append_call_param(params, funcsig, tnreg, arg_type);
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
static void emit_simd_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
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

static x86::Gp emit_bitfield_load_storage(Program &pgm, x86::Mem storage,
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

static void emit_bitfield_store_storage(Program &pgm, x86::Mem storage,
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

static void emit_bitfield_sign_extend(Program &pgm, x86::Gp &value,
    const DataDefSTRUCT::BitFieldInfo &bf)
{
    if ( bf.is_unsigned || bf.bit_width == 0 || bf.bit_width >= 64 )
	return;
    uint32_t shift = (uint32_t)(64 - bf.bit_width);
    pgm.cc.shl(value, imm(shift));
    pgm.cc.sar(value, imm(shift));
}

static x86::Gp emit_bitfield_load(Program &pgm, x86::Mem storage,
    const DataDefSTRUCT::BitFieldInfo &bf, const char *hint)
{
    x86::Gp out = emit_bitfield_load_storage(pgm, storage, bf, hint);
    if ( bf.bit_offset )
	pgm.cc.shr(out, imm((uint32_t)bf.bit_offset));
    emit_and_u64(pgm, out, bitfield_mask(bf.bit_width), hint);
    emit_bitfield_sign_extend(pgm, out, bf);
    return out;
}

static x86::Gp emit_bitfield_store_reg(Program &pgm, x86::Mem storage,
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

static x86::Gp emit_bitfield_store_operand(Program &pgm, x86::Mem storage,
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

// Load a sub-qword Mem into a full 64-bit Gp with proper sign/zero extension.
// Plain cc.mov(Gp64, Mem<2>) is not a legal x86 encoding — asmjit silently
// drops it or emits a 16-bit partial op, leaving the upper bits dirty and
// producing wrong arithmetic results.
static void load_mem_to_gpq(Program &pgm, x86::Gp &gp, const x86::Mem &mem, DataDef *type)
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

static void load_mem_to_xmm(Program &pgm, x86::Xmm &xmm, const x86::Mem &mem, DataDef *type)
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

static void store_xmm_to_mem(Program &pgm, x86::Mem &mem, x86::Xmm &xmm, DataDef *type)
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

// compile the increment operator.
// Supports plain variables (fast in-register inc) and struct members /
// *deref (load-inc-store). `left` = postfix (x++), `right` = prefix (++x).
// Shared shape for safeinc / safedec: in-place on the register/Mem.
typedef void (Program::*SafeUnaryStep)(asmjit::Operand &);

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
static Operand &emit_inc_dec(Program &pgm, TokenBase *target, bool postfix,
			     SafeUnaryStep step, regdefp_t &regdp, Operand &_operand,
			     const char *old_name_hint, const char *new_name_hint,
			     const char *op_name)
{
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

Operand &TokenInc::compile(Program &pgm, regdefp_t &regdp)
{
    TokenBase *target = left ? left : right;
    bool postfix = (left != nullptr);
    if ( !target ) pgm.Throw(this) << "Invalid increment" << flush;
    return emit_inc_dec(pgm, target, postfix, &Program::safeinc, regdp, _operand,
			"postinc", "preinc", "++");
}

Operand &TokenDec::compile(Program &pgm, regdefp_t &regdp)
{
    TokenBase *target = left ? left : right;
    bool postfix = (left != nullptr);
    if ( !target ) pgm.Throw(this) << "Invalid decrement" << flush;
    return emit_inc_dec(pgm, target, postfix, &Program::safedec, regdp, _operand,
			"postdec", "predec", "--");
}

/////////////////////////////////////////////////////////////////////////////
// compound assignment operators (+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)
// Pattern: resolve LHS, compile RHS normalized, apply safe op in place,
// write back + route result through finish_compound_assign.
/////////////////////////////////////////////////////////////////////////////

// Helper for +=, -=, *= (3-arg safe ops with DataDef slots).
static Operand &emit_compound_binop3(Program &pgm, TokenBase *left, TokenBase *right,
				     SafeBinOp3 op, regdefp_t &regdp, Operand &_operand,
				     const char *op_name)
{
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

Operand &TokenAddEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "+= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "+= missing rval operand" << flush;
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_addsub(pgm, left, right, /*subtract=*/false, regdp, _operand);
    return emit_compound_binop3(pgm, left, right, &Program::safeadd, regdp, _operand, "+=");
}

Operand &TokenSubEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "-= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "-= missing rval operand" << flush;
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_addsub(pgm, left, right, /*subtract=*/true, regdp, _operand);
    return emit_compound_binop3(pgm, left, right, &Program::safesub, regdp, _operand, "-=");
}

Operand &TokenMulEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "*= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "*= missing rval operand" << flush;
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_muldiv(pgm, left, right, /*divide=*/false, regdp, _operand);
    return emit_compound_binop3(pgm, left, right, &Program::safemul, regdp, _operand, "*=");
}

Operand &TokenDivEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "/= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "/= missing rval operand" << flush;
    if ( left->datadef() && left->datadef()->is_complex() )
	return emit_complex_compound_muldiv(pgm, left, right, /*divide=*/true, regdp, _operand);
    return emit_compound_divmod(pgm, left, right, /*return_remainder=*/false, regdp, _operand, "/=");
}

Operand &TokenModEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "%= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "%= missing rval operand" << flush;
    return emit_compound_divmod(pgm, left, right, /*return_remainder=*/true, regdp, _operand, "%=");
}

Operand &TokenBSLEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "<<= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "<<= missing rval operand" << flush;
    return emit_compound_bitop2(pgm, left, right, &Program::safeshl, regdp, _operand, "<<=");
}

Operand &TokenBSREq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << ">>= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << ">>= missing rval operand" << flush;
    return emit_compound_bitop2(pgm, left, right, &Program::safeshr, regdp, _operand, ">>=");
}

Operand &TokenBandEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "&= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "&= missing rval operand" << flush;
    return emit_compound_bitop2(pgm, left, right, &Program::safeand, regdp, _operand, "&=");
}

Operand &TokenBorEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "|= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "|= missing rval operand" << flush;
    return emit_compound_bitop2(pgm, left, right, &Program::safeor, regdp, _operand, "|=");
}

Operand &TokenXorEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "^= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "^= missing rval operand" << flush;
    return emit_compound_bitop2(pgm, left, right, &Program::safexor, regdp, _operand, "^=");
}

static void emit_raw_aggregate_copy(Program &pgm, Operand &dst, Operand &src,
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
    return pgm.tkFunction->voperand(pgm, &var);
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

static void bind_call_return(Program &pgm, InvokeNode *call, Operand *dest, DataDef *ret_type,
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


// addition
Operand &TokenAdd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenAdd::Compile({" << (uint64_t)regdp.first << ", " << (uint64_t)regdp.second << "}) TOP" << endl);
    if ( !left )  { throw "+ missing lval operand"; }
    if ( !right ) { throw "+ missing rval operand"; }
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


/////////////////////////////////////////////////////////////////////////////
// comparison operators                                                    //
/////////////////////////////////////////////////////////////////////////////


// Equal to: ==
Operand &TokenEquals::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "== missing lval operand"; }
    if ( !right ) { throw "== missing rval operand"; }
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Eq, regdp, _operand);
}

// Not equal to: !=
Operand &TokenNotEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "!= missing lval operand"; }
    if ( !right ) { throw "!= missing rval operand"; }
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Ne, regdp, _operand);
}

// Less than: <
Operand &TokenLT::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "< missing lval operand"; }
    if ( !right ) { throw "< missing rval operand"; }
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Lt, regdp, _operand);
}

// Less than or equal to: <=
Operand &TokenLE::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "<= missing lval operand"; }
    if ( !right ) { throw "<= missing rval operand"; }
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Le, regdp, _operand);
}

// Greater than: >
Operand &TokenGT::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw "> missing lval operand"; }
    if ( !right ) { throw "> missing rval operand"; }
    if ( can_optimize() ) { return optimize(pgm, regdp); }
    return emit_compare(pgm, left, right, CmpKind::Gt, regdp, _operand);
}

// Greater than or equal to: >=
Operand &TokenGE::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  { throw ">= missing lval operand"; }
    if ( !right ) { throw ">= missing rval operand"; }
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

static DataDef *complex_component_type(DataDef *dd)
{
    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(dd);
    return cdd && cdd->element_type ? cdd->element_type : dd;
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

// compile a return statement
Operand &TokenRETURN::compile(Program &pgm, regdefp_t &regdp)
{
    FuncDef *current_func = pgm.tkFunction && pgm.tkFunction->method
	? dynamic_cast<FuncDef *>(pgm.tkFunction->method->returns.type)
	: NULL;

    // multi-return: write values to __retbuf and return without cleanup
    // (cleanup runs destructors which can't be called multiple times
    //  when there are multiple return paths in if/else branches)
    if ( !return_exprs.empty() )
    {
	DBG(pgm.cc.comment("multi-return: writing values to __retbuf"));
	// find __retbuf parameter
	std::string rbname = "__retbuf";
	Variable *rbvar = pgm.tkFunction->method ? pgm.tkFunction->method->findParameter(rbname) : NULL;
	if ( !rbvar )
	    throw "multi-return: __retbuf parameter not found";
	Operand &rb_op = pgm.tkFunction->voperand(pgm, rbvar);
	x86::Gp rb_gp = pgm.cc.newIntPtr("__retbuf_gp");
	load_var_to_gp(pgm, rb_op, rb_gp);
	FuncDef *fdef = pgm.tkFunction && pgm.tkFunction->method
	    ? dynamic_cast<FuncDef *>(pgm.tkFunction->method->returns.type)
	    : NULL;
	IRBuilder ir(pgm.cc);

	for ( size_t i = 0; i < return_exprs.size(); ++i )
	{
	    DataDef *slot_type = (fdef && i < fdef->return_types.size())
		? fdef->return_types[i]
		: &ddINT64;
	    Operand ret_storage;
	    regdefp_t retrdp = {NULL, NULL, NULL};
	    bool ir_slot = slot_type && (slot_type->is_numeric() || slot_type->is_pointer());
	    Operand &val = ir_slot
		? compile_token_normalized(pgm, return_exprs[i], slot_type, NULL, ret_storage)
		: return_exprs[i]->compile(pgm, retrdp);
	    if ( ir_slot )
	    {
		x86::Mem slot = x86::qword_ptr(rb_gp, (int32_t)(i * 8));
		slot.setSize(8);
		ir.store(IRValue::mem(slot, slot_type), ir_from_operand(val, slot_type));
	    }
	    else if ( val.isReg() && val.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.mov(x86::qword_ptr(rb_gp, (int32_t)(i * 8)), val.as<x86::Gp>());
	    else if ( val.isReg() && val.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.cc.movsd(x86::qword_ptr(rb_gp, (int32_t)(i * 8)), val.as<x86::Xmm>());
	    else if ( val.isImm() )
	    {
		x86::Gp tmp = pgm.cc.newGpq("__ret_tmp");
		pgm.cc.mov(tmp, val.as<Imm>());
		pgm.cc.mov(x86::qword_ptr(rb_gp, (int32_t)(i * 8)), tmp);
	    }
	    else if ( val.isMem() )
	    {
		x86::Gp tmp = pgm.cc.newGpq("__ret_tmp");
		pgm.cc.mov(tmp, val.as<x86::Mem>());
		pgm.cc.mov(x86::qword_ptr(rb_gp, (int32_t)(i * 8)), tmp);
	    }
	}
	emit_function_instrument_exit(pgm, current_func);
	pgm.cc.ret();
	return _reg;
    }

    // single-return or void: cleanup before returning
    pgm.tkFunction->cleanup(pgm);

    if ( returns )
    {
	DataDef *ret_type = &ddVOID;
	if ( pgm.tkFunction && pgm.tkFunction->method )
	{
	    FuncDef *fdef = dynamic_cast<FuncDef *>(pgm.tkFunction->method->returns.type);
	    if ( fdef )
		ret_type = &fdef->returns;
	}

	// char* / const char* returns from string literals or string-valued
	// expressions must return the underlying C string buffer, not the
	// std::string object address.
	if ( type_is_cstr_pointer(ret_type)
	  && returns->datadef() && returns->datadef()->rawtype() == DataType::dtSTRING )
	{
	    Operand ret_storage;
	    Operand &cstr_gp = compile_token_normalized(pgm, returns, ret_type, nullptr, ret_storage);
	    _operand = cstr_gp;
	    emit_function_instrument_exit(pgm, current_func);
	    pgm.saferet(cstr_gp);
	    return _operand;
	}

	// Void return-of-expression: `return some_void_call();` or
	// `return (void)expr;`. C allows this in a void-returning function;
	// the inner expression must run for side effects, but there's no
	// value to ret. Compile the expression, drop the result, emit a
	// bare ret. Without this, saferet would receive an empty Operand
	// from the void-call's compile and throw.
	if ( ret_type && ret_type->rawtype() == DataType::dtVOID
	  && !ret_type->is_pointer() )
	{
	    // `return some_void_call();` in a void-returning function: run
	    // the inner expression for side effects, drop the (empty) result,
	    // emit a bare ret. Without this saferet would receive an empty
	    // Operand and throw. The is_pointer guard keeps `void *` returns
	    // (which share rawtype dtVOID with bare void) on the regular
	    // pointer path.
	    regdefp_t void_rdp = {NULL, NULL, NULL};
	    returns->compile(pgm, void_rdp);
	    emit_function_instrument_exit(pgm, current_func);
	    emit_zeroed_void_return(pgm);
	    return _reg;
	}

	Operand ret_storage;
	Operand &reg = (ret_type && (ret_type->is_numeric() || ret_type->is_pointer()))
	    ? compile_token_normalized(pgm, returns, ret_type, NULL, ret_storage)
	    : returns->compile(pgm, regdp);

	if ( ret_type
	  && ret_type->basetype() == BaseType::btStruct
	  && ret_type->size > 0 && ret_type->size <= 16 )
	{
	    emit_function_instrument_exit(pgm, current_func);
	    emit_small_struct_return(pgm, reg, ret_type);
	    return reg;
	}

	// Large struct return: copy return value into hidden __retbuf
	if ( is_large_struct_return(ret_type) )
	{
	    DBG(pgm.cc.comment("large struct return: copy to __retbuf"));
	    std::string rbname = "__retbuf";
	    Variable *retbuf_var = pgm.tkFunction->method
		? pgm.tkFunction->method->findParameter(rbname) : NULL;
	    if ( !retbuf_var )
		pgm.Throw(this) << "Large struct return: missing __retbuf parameter" << flush;
	    Operand &retbuf_op = pgm.tkFunction->voperand(pgm, retbuf_var);
	    x86::Gp rb_gp = pgm.cc.newIntPtr("__retbuf_gp");
	    load_var_to_gp(pgm, retbuf_op, rb_gp);
	    Operand rb_dst = rb_gp;
	    emit_raw_aggregate_copy(pgm, rb_dst, reg, ret_type, "large_struct_ret");
	    emit_function_instrument_exit(pgm, current_func);
	    pgm.cc.ret();
	    return reg;
	}

	emit_function_instrument_exit(pgm, current_func);
	pgm.saferet(reg);
	return reg;
    }
    emit_function_instrument_exit(pgm, current_func);
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
    // `continue` jumps to the innermost ENCLOSING LOOP's continue
    // label. Switches push (NULL, exit) onto loopstack so `break`
    // exits the switch — but `continue` inside a switch must skip
    // the switch and target the enclosing loop. Walk the stack from
    // top to bottom looking for the first entry with a non-NULL
    // continue label (which only loops, not switches, set).
    DBG(pgm.cc.comment("CONTINUE"));
    std::stack<std::pair<asmjit::Label *, asmjit::Label *>> tmp = pgm.loopstack;
    while ( !tmp.empty() )
    {
	auto &top = tmp.top();
	if ( top.first != NULL )
	{
	    pgm.cc.jmp(*top.first);
	    break;
	}
	tmp.pop();
    }
    return _reg;
}

// Function-scope label-map accessor. Creates an asmjit Label on
// first reference so forward-referenced gotos resolve without
// ordering requirements. `TokenFunc::compile` clears
// `pgm.label_map` at each function boundary.
static asmjit::Label &lookup_or_make_label(Program &pgm, const std::string &name)
{
    auto it = pgm.label_map.find(name);
    if ( it != pgm.label_map.end() )
	return it->second;
    pgm.label_map[name] = pgm.cc.newLabel();
    return pgm.label_map[name];
}

// compile a goto statement
Operand &TokenGOTO::compile(Program &pgm, regdefp_t &regdp)
{
    if ( indirect_target )
    {
	DBG(std::cout << "TokenGOTO::compile(*expr)" << std::endl);
	regdefp_t jump_rdp = {NULL, NULL, NULL};
	Operand &jump_op = indirect_target->compile(pgm, jump_rdp);
	x86::Gp jump_gp = as_gp_ptr(pgm, jump_op, "goto_indirect");
	DBG(pgm.cc.comment("goto *expr"));
	pgm.cc.jmp(jump_gp);
	return _reg;
    }

    DBG(std::cout << "TokenGOTO::compile(" << target << ")" << std::endl);
    asmjit::Label &L = lookup_or_make_label(pgm, target);
    DBG(pgm.cc.comment(("goto " + target).c_str()));
    pgm.cc.jmp(L);
    return _reg;
}

// compile a label definition: bind the Label at the current point.
Operand &TokenLabel::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenLabel::compile(" << name << ":)" << std::endl);
    asmjit::Label &L = lookup_or_make_label(pgm, name);
    DBG(pgm.cc.comment((name + ":").c_str()));
    pgm.cc.bind(L);
    return _reg;
}

// compile a switch statement
Operand &TokenSWITCH::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenSWITCH::compile() TOP — " << cases.size() << " cases" << std::endl);
    DBG(pgm.cc.comment("switch start"));

    DataDef *expr_type = expression && expression->datadef() ? expression->datadef() : &ddINT64;
    if ( expr_type && expr_type->is_integer() )
	expr_type = promote_c_integer_type(expr_type);
    bool normalized_switch = expr_type && (expr_type->is_integer() || expr_type->is_pointer());
    Operand expr_storage;
    Operand *expr_op = NULL;
    if ( normalized_switch )
	expr_op = &compile_token_gp_normalized(pgm, expression, expr_type, expr_storage);
    else
    {
	regdefp_t exprrdp = {NULL, NULL, NULL};
	Operand &raw_expr = expression->compile(pgm, exprrdp);
	expr_type = exprrdp.second ? exprrdp.second : expr_type;
	x86::Gp expr_reg = pgm.cc.newGpq("switch_expr");
	if ( raw_expr.isReg() && raw_expr.as<BaseReg>().isGroup(RegGroup::kGp) )
	    pgm.safemov(expr_reg, raw_expr.as<x86::Gp>(), &ddINT64, expr_type);
	else if ( raw_expr.isImm() )
	    pgm.cc.mov(expr_reg, raw_expr.as<Imm>());
	else if ( raw_expr.isMem() )
	{
	    IRBuilder ir(pgm.cc);
	    IRValue expr_val = ir.load(ir.coerce(IRValue::mem(raw_expr.as<x86::Mem>(), expr_type), &ddINT64));
	    expr_reg = expr_val.op.as<x86::Gp>();
	}
	expr_storage = expr_reg;
	expr_op = &expr_storage;
	expr_type = &ddINT64;
    }
    x86::Gp expr_reg = expr_op->as<x86::Gp>();

    Label sw_exit = pgm.cc.newLabel();

    // create labels for each case + default
    std::vector<Label> case_labels;
    for ( size_t i = 0; i < cases.size(); ++i )
	case_labels.push_back(pgm.cc.newLabel());
    Label default_label = pgm.cc.newLabel();

    // emit compare-and-jump for each case
    for ( size_t i = 0; i < cases.size(); ++i )
    {
	Operand val_storage;
	if ( cases[i]->range_high )
	{
	    // GNU case range: case LOW ... HIGH:
	    // Check LOW <= expr <= HIGH
	    Label skip = pgm.cc.newLabel();
	    Operand lo_storage, hi_storage;
	    if ( normalized_switch )
	    {
		Operand &lo_op = compile_token_gp_normalized(pgm, cases[i]->value, expr_type, lo_storage);
		pgm.safecmp(*(Operand *)&expr_reg, lo_op, expr_type);
	    }
	    else
	    {
		regdefp_t lordp = {NULL, NULL, NULL};
		Operand &lo_op = cases[i]->value->compile(pgm, lordp);
		pgm.safecmp(*(Operand *)&expr_reg, lo_op, expr_type);
	    }
	    pgm.cc.jb(skip);
	    if ( normalized_switch )
	    {
		Operand &hi_op = compile_token_gp_normalized(pgm, cases[i]->range_high, expr_type, hi_storage);
		pgm.safecmp(*(Operand *)&expr_reg, hi_op, expr_type);
	    }
	    else
	    {
		regdefp_t hirdp = {NULL, NULL, NULL};
		Operand &hi_op = cases[i]->range_high->compile(pgm, hirdp);
		pgm.safecmp(*(Operand *)&expr_reg, hi_op, expr_type);
	    }
	    pgm.cc.jbe(case_labels[i]);
	    pgm.cc.bind(skip);
	}
	else
	{
	if ( normalized_switch )
	{
	    Operand &val_op = compile_token_gp_normalized(pgm, cases[i]->value, expr_type, val_storage);
	    pgm.safecmp(*(Operand *)&expr_reg, val_op, expr_type);
	}
	else
	{
	    regdefp_t valrdp = {NULL, NULL, NULL};
	    Operand &val_op = cases[i]->value->compile(pgm, valrdp);
	    pgm.safecmp(*(Operand *)&expr_reg, val_op, expr_type);
	}
	pgm.cc.je(case_labels[i]);
	}
    }
    // fall through to default or exit
    pgm.cc.jmp(defaultcase ? default_label : sw_exit);

    // push exit label onto loopstack so break works
    pgm.loopstack.push(make_pair((Label *)NULL, &sw_exit));

    auto invalidate_rematerializable_globals = [&]() {
	for ( std::map<Variable *, Operand>::iterator it = pgm.tkFunction->operand_map.begin();
	      it != pgm.tkFunction->operand_map.end(); )
	{
	    Variable *var = it->first;
	    if ( var && var->is_global() && var->data
	      && (var->is_fixed_array()
	       || var->type->basetype() == BaseType::btStruct
	       || var->type->basetype() == BaseType::btClass) )
	    {
		it = pgm.tkFunction->operand_map.erase(it);
		continue;
	    }
	    ++it;
	}
    };

    // emit case bodies in source order; insert default body at its
    // source-order position so fall-through chains match what the user
    // wrote.  If default appears after all cases (default_index ==
    // cases.size()) it's emitted after the loop.  If no default exists
    // (default_index == -1) it's never emitted.
    int dflt_pos = defaultcase ? default_index : -1;
    for ( size_t i = 0; i < cases.size(); ++i )
    {
	if ( defaultcase && (int)i == dflt_pos )
	{
	    pgm.cc.bind(default_label);
	    DBG(pgm.cc.comment("default body"));
	    for ( auto *stmt : defaultcase->statements )
	    {
		invalidate_rematerializable_globals();
		regdefp_t stmtrdp = {NULL, NULL, NULL};
		stmt->compile(pgm, stmtrdp);
	    }
	}
	pgm.cc.bind(case_labels[i]);
	DBG(pgm.cc.comment("case body"));
	for ( auto *stmt : cases[i]->statements )
	{
	    invalidate_rematerializable_globals();
	    regdefp_t stmtrdp = {NULL, NULL, NULL};
	    stmt->compile(pgm, stmtrdp);
	}
    }
    if ( defaultcase && dflt_pos == (int)cases.size() )
    {
	pgm.cc.bind(default_label);
	DBG(pgm.cc.comment("default body"));
	for ( auto *stmt : defaultcase->statements )
	{
	    invalidate_rematerializable_globals();
	    regdefp_t stmtrdp = {NULL, NULL, NULL};
	    stmt->compile(pgm, stmtrdp);
	}
    }

    pgm.loopstack.pop();
    pgm.cc.bind(sw_exit);
    DBG(pgm.cc.comment("switch end"));

    return _operand;
}

// TokenCASE::compile() is not called directly — TokenSWITCH::compile() handles it
Operand &TokenCASE::compile(Program &pgm, regdefp_t &regdp)
{
    return _operand;
}

// rust::match codegen — compare-and-jump chain with no fall-through.
// Mirrors the integer path of TokenSWITCH but emits one branch per
// pattern (OR within an arm) and a tail `jmp match_exit` after each
// arm body. The wildcard arm gets its own label; if no match hits, the
// dispatch jumps directly to that label, otherwise to match_exit.
Operand &TokenMatch::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenMatch::compile() " << arms.size() << " arms"
	    << (wildcard_index >= 0 ? " (with _)" : "") << std::endl);
    DBG(pgm.cc.comment("rust::match start"));

    DataDef *expr_type = expression && expression->datadef()
		      ? expression->datadef() : &ddINT64;
    Operand expr_storage;
    Operand &expr_op = compile_token_gp_normalized(pgm, expression,
						   expr_type, expr_storage);
    x86::Gp expr_reg = expr_op.as<x86::Gp>();

    Label match_exit = pgm.cc.newLabel();
    std::vector<Label> arm_labels;
    arm_labels.reserve(arms.size());
    for ( size_t i = 0; i < arms.size(); ++i )
	arm_labels.push_back(pgm.cc.newLabel());

    // Dispatch: emit cmp+je for every constant pattern in every non-
    // wildcard arm, in source order. After the chain, fall through to
    // the wildcard arm if one exists, otherwise to the exit.
    for ( size_t i = 0; i < arms.size(); ++i )
    {
	if ( arms[i]->is_wildcard )
	    continue;
	for ( size_t p = 0; p < arms[i]->patterns.size(); ++p )
	{
	    Operand val_storage;
	    Operand &val_op = compile_token_gp_normalized(pgm,
		    arms[i]->patterns[p], expr_type, val_storage);
	    pgm.cc.cmp(expr_reg, val_op.as<x86::Gp>());
	    pgm.cc.je(arm_labels[i]);
	}
    }
    if ( wildcard_index >= 0 )
	pgm.cc.jmp(arm_labels[wildcard_index]);
    else
	pgm.cc.jmp(match_exit);

    // break inside an arm body should leave the match — push exit on
    // loopstack the same way TokenSWITCH does.
    pgm.loopstack.push(make_pair((Label *)NULL, &match_exit));

    // Emit each arm body in source order, terminating each with an
    // unconditional jump to match_exit so arms never fall through.
    for ( size_t i = 0; i < arms.size(); ++i )
    {
	pgm.cc.bind(arm_labels[i]);
	DBG(pgm.cc.comment("match arm body"));
	if ( arms[i]->body )
	{
	    regdefp_t armrdp = {NULL, NULL, NULL};
	    arms[i]->body->compile(pgm, armrdp);
	}
	pgm.cc.jmp(match_exit);
    }

    pgm.loopstack.pop();
    pgm.cc.bind(match_exit);
    DBG(pgm.cc.comment("rust::match end"));
    return _operand;
}

// Check if a statement subtree contains any labels (TokenLabel or
// TokenCASE) that might be jump targets.  Used to decide whether an
// if(0) then-block can safely be elided.
static bool contains_label(TokenBase *node)
{
    if ( !node ) return false;
    if ( dynamic_cast<TokenLabel *>(node) ) return true;
    if ( dynamic_cast<TokenCASE *>(node) ) return true;
    if ( TokenCpnd *cpnd = dynamic_cast<TokenCpnd *>(node) )
    {
	for ( TokenStmt *s : cpnd->statements )
	    if ( contains_label(s) )
		return true;
    }
    if ( TokenIF *tif = dynamic_cast<TokenIF *>(node) )
    {
	if ( contains_label(tif->statement) ) return true;
	if ( contains_label(tif->elsestmt) ) return true;
    }
    if ( TokenFOR *tf = dynamic_cast<TokenFOR *>(node) )
	return contains_label(tf->statement);
    if ( TokenWHILE *tw = dynamic_cast<TokenWHILE *>(node) )
	return contains_label(tw->statement);
    if ( TokenDO *td = dynamic_cast<TokenDO *>(node) )
	return contains_label(td->statement);
    if ( TokenFOREACH *tfe = dynamic_cast<TokenFOREACH *>(node) )
	return contains_label(tfe->statement);
    if ( TokenSWITCH *tsw = dynamic_cast<TokenSWITCH *>(node) )
    {
	for ( TokenCASE *c : tsw->cases )
	    if ( contains_label(c) )
		return true;
	if ( contains_label(tsw->defaultcase) )
	    return true;
    }
    return false;
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
    // perform condition check, false goes either to elsedo or iftail.
    // Reset regdp first so the condition's compile doesn't inherit a
    // stale destination register from the caller — same rule applied in
    // TokenFOR / TokenWHILE / TokenDO (see .claude/rules/regdp-reset.md).
    if ( condition
      && (condition->type() == TokenType::ttInteger
       || condition->type() == TokenType::ttChar) )
    {
	if ( condition->ival() )
	{
	    // if(1): skip else, compile then-block only
	    pgm.cc.bind(thendo);
	    regdp.first = NULL; regdp.second = NULL;
	    statement->compile(pgm, regdp);
	}
	else if ( contains_label(statement) )
	{
	    // if(0) but the then-block contains labels (goto targets
	    // or case labels).  Emit it as unreachable code so labels
	    // get bound — matches GCC's flattening behavior.
	    pgm.cc.jmp(elsestmt ? elsedo : iftail);
	    pgm.cc.bind(thendo);
	    regdp.first = NULL; regdp.second = NULL;
	    statement->compile(pgm, regdp);
	    if ( elsestmt )
	    {
		pgm.cc.jmp(iftail);
		pgm.cc.bind(elsedo);
		regdp.first = NULL; regdp.second = NULL;
		elsestmt->compile(pgm, regdp);
	    }
	}
	else if ( elsestmt )
	{
	    // if(0) with no labels: safe to skip then-block entirely
	    pgm.cc.bind(elsedo);
	    regdp.first = NULL; regdp.second = NULL;
	    elsestmt->compile(pgm, regdp);
	}
	pgm.cc.bind(iftail);
	pgm.ifstack.pop();
	return _operand;
    }
    DBG(pgm.cc.comment("TokenIF::compile() reg = condition->compile()"));
    regdp.first  = NULL;
    regdp.second = NULL;
    Operand cond_storage;
    Operand &reg = compile_condition_operand(pgm, condition, cond_storage);
    // hard coded if (1) / if (0)
    if ( reg.isImm() )
    {
	// if (1) (or any non-zero)
	if ( reg.as<Imm>().value() )
	{
	    pgm.cc.bind(thendo);
	    DBG(pgm.cc.comment("TokenIF::compile(1) statement->compile(pgm, regdp)"));
	    regdp.first = NULL; regdp.second = NULL;
	    statement->compile(pgm, regdp); // execute if statement(s) for true
	}
	else
	// if (0)
	if ( elsestmt )
	{
	    pgm.cc.bind(elsedo);	// bind elsedo label
	    DBG(pgm.cc.comment("TokenIF::compile(0) elsestmt->compile(pgm, regdp)"));
	    regdp.first = NULL; regdp.second = NULL;
	    elsestmt->compile(pgm, regdp);  // execute else condition
	}
    }
    // logic controlled
    else
    if ( reg.isReg() || reg.isMem() )
    {
	DBG(cout << "TokenIF::compile() pgm.safetest(reg, reg)" << endl);
	DBG(pgm.cc.comment("TokenIF::compile() pgm.safetest(reg, reg)"));
	pgm.testzero(reg); //pgm.safetest(reg, reg);			// compare to zero
	DBG(pgm.cc.comment("TokenIF::compile() pgm.cc.je(else/tail)"));
	pgm.cc.je(elsestmt ? elsedo : iftail);	// jump appropriately

	DBG(pgm.cc.comment("TokenIF::compile() statement->compile(pgm, regdp)"));
	pgm.cc.bind(thendo);
	regdp.first = NULL; regdp.second = NULL;
	statement->compile(pgm, regdp); // execute if statement(s) if condition met
	if ( elsestmt )			// do we have an else?
	{
	    pgm.cc.jmp(iftail);		// jump to tail after executing if statements
	    pgm.cc.bind(elsedo);	// bind elsedo label
	    DBG(pgm.cc.comment("TokenIF::compile() elsestmt->compile(pgm, regdp)"));
	    regdp.first = NULL; regdp.second = NULL;
	    elsestmt->compile(pgm, regdp); 	// execute else condition
	}
    }
    else
	throw "TokenIF::compile() condition->compile() didn't return a usable operand";
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
    regdp.first  = NULL;			// reset regdp for body
    regdp.second = NULL;
    statement->compile(pgm, regdp); 	// execute loop's statement(s)
    DBG(cout << "TokenDO::compile() statement done, compiling condition" << endl);
    regdp.first  = NULL;			// reset regdp for condition
    regdp.second = NULL;
    Operand cond_storage;
    Operand &reg = compile_condition_operand(pgm, condition, cond_storage); // get condition result
    DBG(cout << "TokenDO::compile() condition done, reg type: " << (reg.isReg() ? "reg" : reg.isMem() ? "mem" : "other") << endl);
    DBG(pgm.cc.comment("TokenDO::compile() pgm.safetest(reg, reg)"));
    pgm.testzero(reg);			// compare to zero
    DBG(cout << "TokenDO::compile() testzero done" << endl);
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
    regdp.first  = NULL;			// reset regdp for condition
    regdp.second = NULL;
    DBG(pgm.cc.comment("condition->compile(pgm, regdp)"));
    Operand cond_storage;
    Operand &reg = compile_condition_operand(pgm, condition, cond_storage);// get condition result
    DBG(pgm.cc.comment("TokenWHILE::compile() pgm.safetest(reg, reg)"));
    pgm.testzero(reg);  //    pgm.safetest(reg, reg);			// compare to zero
    pgm.cc.je(whiletail);			// if zero, jump to end

    DBG(cout << "TokenWHILE::compile() calling statement->compile(pgm, regdp)" << endl);
    pgm.cc.bind(whiledo);			// bind action label
    regdp.first  = NULL;			// reset regdp for body
    regdp.second = NULL;
    DBG(pgm.cc.comment("statement->compile(pgm, regdp)"));
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
    if ( initialize )
	initialize->compile(pgm, regdp); 	// execute loop's initializer statement
    for ( auto *extra : init_extras )		// C comma-init extras
    {
	regdp.first = NULL;
	regdp.second = NULL;
	extra->compile(pgm, regdp);
    }
    pgm.cc.bind(fortop);			// label the top of the loop
    regdp.first  = NULL;			// reset so condition compiles into a fresh register
    regdp.second = NULL;
    Operand cond_storage;
    Operand &reg = compile_condition_operand(pgm, condition, cond_storage); // get condition result
    DBG(pgm.cc.comment("TokenFOR::compile() pgm.safetest(reg, reg)"));
    pgm.testzero(reg);				// compare to zero
    pgm.cc.je(fortail);				// jump to end

    DBG(cout << "TokenFOR::compile() calling statement->compile(pgm, regdp)" << endl);
    regdp.first  = NULL;			// reset so statement doesn't inherit stale destination
    regdp.second = NULL;
    statement->compile(pgm, regdp); 		// execute loop's statement(s)
    pgm.cc.bind(forcont);			// bind continue label
    regdp.first  = NULL;			// reset so increment doesn't clobber unrelated registers
    regdp.second = NULL;
    if ( increment )
	increment->compile(pgm, regdp); 		// execute loop's increment statement
    for ( auto *extra : incr_extras )		// C comma-incr extras
    {
	regdp.first = NULL;
	regdp.second = NULL;
	extra->compile(pgm, regdp);
    }
    pgm.cc.jmp(fortop);				// jump back to top
    pgm.cc.bind(fortail);			// bind for tail

    pgm.loopstack.pop();			// pop labels from loopstack
    DBG(std::cout << "TokenFOR::compile() END" << std::endl);

    return reg;
}

Operand &TokenFOREACH::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(std::cout << "TokenFOREACH::compile() TOP — " << elemtype->name << ' ' << elemname << std::endl);
    DBG(pgm.cc.comment("TokenFOREACH::compile() start"));

    Label fortop  = pgm.cc.newLabel();
    Label forcont = pgm.cc.newLabel();
    Label fortail = pgm.cc.newLabel();

    pgm.loopstack.push(make_pair(&forcont, &fortail));

    // compile container expression to get array pointer
    regdefp_t arrrdp = {NULL, NULL, NULL};
    Operand &arr_op = container->compile(pgm, arrrdp);
    x86::Gp arr_reg = pgm.cc.newIntPtr("foreach_arr");
    pgm.cc.mov(arr_reg, arr_op.as<x86::Gp>());

    // determine the container type to pick the right size/at functions
    DataDef *container_type = arrrdp.second;
    bool is_vector = container_type && container_type->type() == DataType::dtVECTOR;

    // get count — dispatch based on container type
    x86::Gp count_reg = pgm.cc.newGpq("foreach_count");
    {
	void *size_fn;
	if ( is_vector )
	    size_fn = elemtype->is_string() ? (void *)vector_str_size : (void *)vector_int_size;
	else
	    size_fn = (void *)php_count;
	InvokeNode *cnt_call;
	pgm.cc.invoke(&cnt_call, imm(size_fn), FuncSignature::build<int64_t, void *>());
	cnt_call->setArg(0, arr_reg);
	cnt_call->setRet(0, count_reg);
    }

    // index register
    x86::Gp idx_reg = pgm.cc.newGpq("foreach_idx");
    pgm.cc.xor_(idx_reg, idx_reg);

    // allocate loop variable
    TokenCpnd *code = pgm.tkFunction;
    Operand &elem_op = code->voperand(pgm, elemvar);

    // loop top
    pgm.cc.bind(fortop);
    pgm.cc.cmp(idx_reg, count_reg);
    pgm.cc.jge(fortail);

    // fetch element — dispatch based on container + element type
    if ( elemtype->is_string() )
    {
	void *at_fn = is_vector ? (void *)vector_str_at : (void *)php_array_get;
	DBG(pgm.cc.comment("foreach: get string element"));
	InvokeNode *get_call;
	pgm.cc.invoke(&get_call, imm(at_fn),
	    FuncSignature::build<void *, void *, void *, int64_t>());
	get_call->setArg(0, elem_op.as<x86::Gp>());
	get_call->setArg(1, arr_reg);
	get_call->setArg(2, idx_reg);
    }
    else if ( elemtype->is_integer() )
    {
	void *at_fn = is_vector ? (void *)vector_int_at : (void *)php_array_get_int;
	DBG(pgm.cc.comment("foreach: get int element"));
	InvokeNode *get_call;
	pgm.cc.invoke(&get_call, imm(at_fn),
	    FuncSignature::build<int64_t, void *, int64_t>());
	get_call->setArg(0, arr_reg);
	get_call->setArg(1, idx_reg);
	get_call->setRet(0, elem_op.as<x86::Gp>());
    }
    else
    {
	pgm.Throw(this) << "range-for: unsupported element type '" << elemtype->name << "'" << flush;
    }

    // loop body
    statement->compile(pgm, regdp);

    // continue: increment and loop
    pgm.cc.bind(forcont);
    pgm.cc.inc(idx_reg);
    pgm.cc.jmp(fortop);
    pgm.cc.bind(fortail);

    pgm.loopstack.pop();
    DBG(std::cout << "TokenFOREACH::compile() END" << std::endl);

    return _operand;
}
