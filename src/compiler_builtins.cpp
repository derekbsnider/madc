//////////////////////////////////////////////////////////////////////////
//									//
// Builtin function dispatch table and handlers.			//
//									//
// Extracted from compiler.cpp — each __builtin_* function gets its	//
// own handler; the dispatch table replaces the if-chain in		//
// TokenCallFunc::compile().						//
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
#include <vector>
#include <queue>
#include <stack>
#include <map>
#include <unordered_map>
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

// Runtime helpers — actual function implementations called by JIT code.
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

// Handler function type — every builtin handler has this signature.
typedef Operand &(*builtin_emit_fn)(Program &pgm, TokenCallFunc *call,
				    regdefp_t &regdp, Operand &storage);

//////////////////////////////////////////////////////////////////////////
//  Simple libc redirect table						//
//////////////////////////////////////////////////////////////////////////

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

//////////////////////////////////////////////////////////////////////////
//  Overflow helpers							//
//////////////////////////////////////////////////////////////////////////

static std::string runtime_symbol_name(const Variable &var)
{
    return var.storage_alias_name.empty() ? var.name : var.storage_alias_name;
}

bool is_overflow_predicate_helper_name(const std::string &name)
{
    return name == "__madc_add_overflow_p"
	|| name == "__madc_sub_overflow_p"
	|| name == "__madc_mul_overflow_p";
}

bool is_overflow_store_helper_name(const std::string &name)
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

std::string typed_overflow_predicate_symbol_name(const Variable &var,
						 const std::vector<TokenBase *> &parameters)
{
    if ( !is_overflow_predicate_helper_name(var.name) || parameters.size() < 3 )
	return runtime_symbol_name(var);
    DataDef *indicator_type = overflow_predicate_indicator_type(parameters);
    return var.name + "_" + overflow_predicate_helper_suffix(indicator_type);
}

DataDef *overflow_store_result_type(const std::vector<TokenBase *> &parameters)
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

std::string typed_overflow_store_symbol_name(const Variable &var,
					     const std::vector<TokenBase *> &parameters)
{
    if ( !is_overflow_store_helper_name(var.name) || parameters.size() < 3 )
	return runtime_symbol_name(var);
    DataDef *result_type = overflow_store_result_type(parameters);
    if ( result_type && result_type->is_unsigned() && result_type->size >= 8
      && overflow_inputs_are_unsigned64(parameters) )
	return var.name + "_uu64";
    return var.name + "_" + overflow_store_helper_suffix(result_type);
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

//////////////////////////////////////////////////////////////////////////
//  Abs family helpers							//
//////////////////////////////////////////////////////////////////////////

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

//////////////////////////////////////////////////////////////////////////
//  Complex builtin helpers						//
//////////////////////////////////////////////////////////////////////////

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

//////////////////////////////////////////////////////////////////////////
//  Setjmp / longjmp / alloca handlers					//
//////////////////////////////////////////////////////////////////////////

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

// Alloca pool infrastructure
static const uint32_t DEFAULT_ALLOCA_POOL_SIZE = 65536;

static uint32_t get_alloca_pool_size()
{
    static uint32_t cached = 0;
    if ( !cached )
    {
	const char *env = ::getenv("MADC_ALLOCA_POOL_SIZE");
	cached = (env && atoi(env) > 0) ? (uint32_t)atoi(env) : DEFAULT_ALLOCA_POOL_SIZE;
    }
    return cached;
}

static void ensure_alloca_pool(Program &pgm)
{
    TokenCpnd *tf = pgm.tkFunction;
    if ( !tf || tf->has_alloca_pool )
	return;

    uint32_t pool_size = get_alloca_pool_size();

    x86::Mem pool = pgm.cc.newStack(pool_size, 16);
    tf->alloca_cursor_slot = pgm.cc.newStack(8, 8);
    tf->alloca_cursor_slot.setSize(8);
    tf->alloca_pool_end_slot = pgm.cc.newStack(8, 8);
    tf->alloca_pool_end_slot.setSize(8);

    BaseNode *saved_cursor = NULL;
    if ( tf->prologue_cursor )
	saved_cursor = pgm.cc.setCursor(tf->prologue_cursor);

    DBG(pgm.cc.comment("alloca pool init"));
    x86::Gp tmp = pgm.cc.newIntPtr("__pool_init");
    pgm.cc.lea(tmp, pool);
    pgm.cc.mov(tf->alloca_cursor_slot, tmp);
    pgm.cc.add(tmp, imm(pool_size));
    pgm.cc.mov(tf->alloca_pool_end_slot, tmp);

    if ( saved_cursor )
    {
	tf->prologue_cursor = pgm.cc.cursor();
	pgm.cc.setCursor(saved_cursor);
    }

    tf->has_alloca_pool = true;
}

static Operand &emit_builtin_alloca(Program &pgm, TokenCallFunc *call,
				    regdefp_t &regdp, Operand &fallback_operand)
{
    if ( call->argc() != 1 )
	pgm.Throw(call) << "__builtin_alloca expects one argument" << flush;

    DBG(pgm.cc.comment("__builtin_alloca"));

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

    ensure_alloca_pool(pgm);

    // Align to 16 bytes
    pgm.cc.add(size_gp, imm(15));
    pgm.cc.and_(size_gp, imm(~(int64_t)15));

    TokenCpnd *tf = pgm.tkFunction;

    x86::Gp result = pgm.cc.newIntPtr("alloca_ptr");
    pgm.cc.mov(result, tf->alloca_cursor_slot);

    x86::Gp new_cursor = pgm.cc.newIntPtr("alloca_new_cur");
    pgm.cc.mov(new_cursor, result);
    pgm.cc.add(new_cursor, size_gp);

    // Overflow check
    x86::Gp pool_end = pgm.cc.newIntPtr("alloca_end");
    pgm.cc.mov(pool_end, tf->alloca_pool_end_slot);
    Label ok = pgm.cc.newLabel();
    pgm.cc.cmp(new_cursor, pool_end);
    pgm.cc.jbe(ok);
    InvokeNode *abort_call;
    pgm.cc.invoke(&abort_call, imm((void *)(intptr_t)::abort),
		   FuncSignature::build<void>());
    pgm.cc.bind(ok);

    pgm.cc.mov(tf->alloca_cursor_slot, new_cursor);

    DataDef *ret_type = pgm.getPointerType(&ddVOID);
    if ( !regdp.second )
	regdp.second = ret_type;
    return emit_ir_value(pgm, IRValue::reg(result, ret_type), regdp, fallback_operand, ret_type);
}

//////////////////////////////////////////////////////////////////////////
//  Individual builtin emit functions					//
//////////////////////////////////////////////////////////////////////////

static Operand &emit_builtin_object_size(Program &pgm, TokenCallFunc *call,
					 regdefp_t &regdp, Operand &storage)
{
    if ( call->argc() != 2 )
	pgm.Throw(call) << "__builtin_object_size expects two arguments" << flush;
    int64_t mode = 0;
    try_eval_const_i64(call->parameters[1], mode);
    int64_t size = estimate_object_size(call->parameters[0], (int)mode);
    regdp.second = &ddUINT64;
    return emit_ir_value(pgm,
	IRValue::imm(Imm(size >= 0 ? size : -1), &ddUINT64),
	regdp, storage, &ddUINT64);
}

static Operand &emit_builtin_frame_address(Program &pgm, TokenCallFunc *call,
					   regdefp_t &regdp, Operand &storage)
{
    DataDef *ret_type = call->returns();
    regdp.second = ret_type;
    x86::Gp frame_gp = pgm.cc.newIntPtr("__builtin_frame_address");
    pgm.cc.mov(frame_gp, x86::rsp);
    return emit_ir_value(pgm, IRValue::reg(frame_gp, ret_type), regdp, storage, ret_type);
}

static Operand &emit_builtin_complex_conjugate_handler(Program &pgm, TokenCallFunc *call,
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

static Operand &emit_builtin_creal_handler(Program &pgm, TokenCallFunc *call,
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
    return emit_ir_value(pgm, ir_from_operand(*real_part, real_type), regdp, storage,
			 cdd->element_type);
}

static Operand &emit_builtin_cimag_handler(Program &pgm, TokenCallFunc *call,
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
    return emit_ir_value(pgm, ir_from_operand(*imag_part_op, imag_type), regdp, storage,
			 cdd->element_type);
}

//////////////////////////////////////////////////////////////////////////
//  __builtin_shuffle handler						//
//////////////////////////////////////////////////////////////////////////

static Operand &emit_builtin_shuffle(Program &pgm, TokenCallFunc *call,
				     regdefp_t &regdp, Operand &storage)
{
    // Resolve the source vector SIMD type
    DataDef *src_dd = call->parameters[0]->datadef();
    DataDefSIMD *vdd = src_dd ? dynamic_cast<DataDefSIMD *>(src_dd) : nullptr;
    if ( !vdd )
	pgm.Throw(call) << "__builtin_shuffle: first argument must be a SIMD vector" << flush;

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
	Operand &src_op = call->parameters[0]->compile(pgm, src_rdp);
	if ( src_op.isReg() && src_op.as<BaseReg>().isGroup(RegGroup::kVec) )
	    store_xmm_to_mem(pgm, src_slot, src_op.as<x86::Xmm>(), vdd);
	else if ( src_op.isMem() )
	{
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
	    pgm.Throw(call) << "__builtin_shuffle: unsupported source operand shape" << flush;
    }

    // For 2-source shuffle (argc==3), spill second source too
    x86::Mem src2_slot;
    bool two_source = (call->argc() == 3);
    if ( two_source )
    {
	src2_slot = new_large_simd_stack(pgm, vdd, "shuf_src2_slot");
	regdefp_t src2_rdp = {nullptr, vdd, nullptr};
	Operand &src2_op = call->parameters[1]->compile(pgm, src2_rdp);
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
    TokenBase *mask_param = call->parameters[call->argc() - 1];
    DataDef *mask_dd = mask_param->datadef();
    DataDefSIMD *mask_vdd = mask_dd ? dynamic_cast<DataDefSIMD *>(mask_dd) : nullptr;
    if ( !mask_vdd )
	pgm.Throw(call) << "__builtin_shuffle: mask must be a SIMD vector" << flush;
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
	    x86::Gp raw_idx = load_simd_lane_gp(pgm, mask_slot, i, mask_vdd, "shuf_raw_idx");
	    pgm.cc.and_(raw_idx, imm((int64_t)(mask_mod - 1)));

	    base_ptr = pgm.cc.newIntPtr("shuf_base_sel");
	    pgm.cc.mov(base_ptr, src_base);
	    pgm.cc.cmp(raw_idx, imm((int64_t)lanes));
	    Label use_src1 = pgm.cc.newLabel();
	    pgm.cc.jb(use_src1);
	    pgm.cc.mov(base_ptr, src2_base);
	    pgm.cc.sub(idx, imm((int64_t)(lanes * elem_size)));
	    pgm.cc.bind(use_src1);
	}
	else
	    base_ptr = src_base;

	// Compute final element address
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
	x86::Xmm dst_xmm = regdp.first->as<x86::Xmm>();
	if ( vdd->size == 8 )
	    pgm.cc.movq(dst_xmm, result_mem);
	else
	    pgm.cc.movups(dst_xmm, result_mem);
    }
    else if ( regdp.first && regdp.first->isMem() )
    {
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
	if ( vdd->size <= 16 )
	{
	    x86::Xmm result_xmm = pgm.cc.newXmm("shuf_result");
	    if ( vdd->size == 8 )
		pgm.cc.movq(result_xmm, result_mem);
	    else
		pgm.cc.movups(result_xmm, result_mem);
	    storage = result_xmm;
	}
	else
	    storage = result_mem;
	regdp.first = &storage;
    }
    return storage;
}

//////////////////////////////////////////////////////////////////////////
//  _chk family — consolidated handlers					//
//////////////////////////////////////////////////////////////////////////

// Shared helper for _chk builtins that strip the size/flag guard when
// constant folding proves the call is safe.  Handles the common pattern:
//   if (len <= size)  → redirect to plain_name(args without size)
//   else              → redirect to chk_name(args with size)

static Operand &emit_chk_mem_redirect(Program &pgm, TokenCallFunc *call,
				      const char *plain_name, const char *chk_name,
				      regdefp_t &regdp, Operand &storage)
{
    // Pattern: builtin(dst, src_or_val, len, size)
    //   args[2] = len, args[3] = size
    int64_t len = -1;
    int64_t size = -1;
    bool len_const = try_eval_const_i64(call->parameters[2], len);
    bool size_const = try_eval_const_i64(call->parameters[3], size);
    bool use_plain = (size_const && size < 0)
	|| (len_const && size_const && len >= 0 && size >= 0
	 && (uint64_t)len <= (uint64_t)size);
    std::vector<TokenBase *> redirected_args;
    redirected_args.push_back(call->parameters[0]);
    redirected_args.push_back(call->parameters[1]);
    redirected_args.push_back(call->parameters[2]);
    if ( !use_plain )
	redirected_args.push_back(call->parameters[3]);
    return redirect_builtin_call(pgm, use_plain ? plain_name : chk_name,
				 redirected_args, regdp, call, storage);
}

static Operand &emit_chk_str2_redirect(Program &pgm, TokenCallFunc *call,
					const char *plain_name, const char *chk_name,
					regdefp_t &regdp, Operand &storage)
{
    // Pattern: builtin(dst, src, size)
    //   args[2] = size
    int64_t size = -1;
    bool size_const = try_eval_const_i64(call->parameters[2], size);
    bool use_plain = size_const && size < 0;
    std::vector<TokenBase *> redirected_args;
    redirected_args.push_back(call->parameters[0]);
    redirected_args.push_back(call->parameters[1]);
    if ( !use_plain )
	redirected_args.push_back(call->parameters[2]);
    return redirect_builtin_call(pgm, use_plain ? plain_name : chk_name,
				 redirected_args, regdp, call, storage);
}

// Individual _chk wrappers — one table entry each.

static Operand &emit_chk_memset(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_mem_redirect(pgm, call, "memset", "__memset_chk", regdp, s); }

static Operand &emit_chk_memcpy(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_mem_redirect(pgm, call, "memcpy", "__memcpy_chk", regdp, s); }

static Operand &emit_chk_memmove(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_mem_redirect(pgm, call, "memmove", "__memmove_chk", regdp, s); }

static Operand &emit_chk_mempcpy(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_mem_redirect(pgm, call, "mempcpy", "__mempcpy_chk", regdp, s); }

static Operand &emit_chk_strncpy(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_mem_redirect(pgm, call, "strncpy", "__strncpy_chk", regdp, s); }

static Operand &emit_chk_strncat(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_mem_redirect(pgm, call, "strncat", "__strncat_chk", regdp, s); }

static Operand &emit_chk_stpncpy(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_mem_redirect(pgm, call, "stpncpy", "__stpncpy_chk", regdp, s); }

static Operand &emit_chk_strcpy(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_str2_redirect(pgm, call, "strcpy", "__strcpy_chk", regdp, s); }

static Operand &emit_chk_stpcpy(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_str2_redirect(pgm, call, "stpcpy", "__stpcpy_chk", regdp, s); }

static Operand &emit_chk_strcat(Program &pgm, TokenCallFunc *call, regdefp_t &regdp, Operand &s)
{ return emit_chk_str2_redirect(pgm, call, "strcat", "__strcat_chk", regdp, s); }

// Printf-family _chk handlers — these have const-eval flag+size logic.

static Operand &emit_chk_vsnprintf(Program &pgm, TokenCallFunc *call,
				   regdefp_t &regdp, Operand &storage)
{
    int64_t flag = 0, len = -1, size = -1;
    bool flag_const = try_eval_const_i64(call->parameters[2], flag);
    bool len_const = try_eval_const_i64(call->parameters[1], len);
    bool size_const = try_eval_const_i64(call->parameters[3], size);
    bool use_plain = flag_const && flag == 0
	&& ((size_const && size < 0)
	 || (len_const && size_const && len >= 0 && size >= 0
	  && (uint64_t)len <= (uint64_t)size));

    std::string target_name = use_plain ? "vsnprintf" : "__vsnprintf_chk";
    std::vector<TokenBase *> args;
    args.push_back(call->parameters[0]);
    args.push_back(call->parameters[1]);
    if ( use_plain )
    {
	args.push_back(call->parameters[4]);
	args.push_back(call->parameters[5]);
    }
    else
    {
	args.push_back(call->parameters[2]);
	args.push_back(call->parameters[3]);
	args.push_back(call->parameters[4]);
	args.push_back(call->parameters[5]);
    }
    return redirect_builtin_call(pgm, target_name, args, regdp, call, storage);
}

static Operand &emit_chk_vsprintf(Program &pgm, TokenCallFunc *call,
				  regdefp_t &regdp, Operand &storage)
{
    int64_t flag = 0, size = -1;
    bool flag_const = try_eval_const_i64(call->parameters[1], flag);
    bool size_const = try_eval_const_i64(call->parameters[2], size);
    int64_t est_len = estimate_constant_printf_output(call->parameters[3], call->parameters, 4);
    bool use_plain = flag_const && flag == 0
	&& ((size_const && size < 0)
	 || (size_const && est_len >= 0 && size >= 0
	  && (uint64_t)est_len < (uint64_t)size));

    std::string target_name = use_plain ? "vsprintf" : "__vsprintf_chk";
    std::vector<TokenBase *> args;
    args.push_back(call->parameters[0]);
    if ( use_plain )
    {
	args.push_back(call->parameters[3]);
	args.push_back(call->parameters[4]);
    }
    else
    {
	args.push_back(call->parameters[1]);
	args.push_back(call->parameters[2]);
	args.push_back(call->parameters[3]);
	args.push_back(call->parameters[4]);
    }
    return redirect_builtin_call(pgm, target_name, args, regdp, call, storage);
}

static Operand &emit_chk_sprintf(Program &pgm, TokenCallFunc *call,
				 regdefp_t &regdp, Operand &storage)
{
    int64_t flag = 0, size = -1;
    bool flag_const = try_eval_const_i64(call->parameters[1], flag);
    bool size_const = try_eval_const_i64(call->parameters[2], size);
    int64_t est_len = estimate_constant_printf_output(call->parameters[3], call->parameters, 4);
    bool use_plain = flag_const && flag == 0
	&& ((size_const && size < 0)
	 || (size_const && est_len >= 0 && size >= 0
	  && (uint64_t)est_len < (uint64_t)size));

    std::string target_name = use_plain ? "sprintf" : "__sprintf_chk";
    std::vector<TokenBase *> args;
    args.push_back(call->parameters[0]);
    if ( use_plain )
    {
	for ( size_t i = 3; i < call->parameters.size(); ++i )
	    args.push_back(call->parameters[i]);
    }
    else
    {
	args.push_back(call->parameters[1]);
	args.push_back(call->parameters[2]);
	for ( size_t i = 3; i < call->parameters.size(); ++i )
	    args.push_back(call->parameters[i]);
    }
    return redirect_builtin_call(pgm, target_name, args, regdp, call, storage);
}

static Operand &emit_chk_snprintf(Program &pgm, TokenCallFunc *call,
				  regdefp_t &regdp, Operand &storage)
{
    int64_t flag = 0, len = -1, size = -1;
    bool flag_const = try_eval_const_i64(call->parameters[2], flag);
    bool len_const = try_eval_const_i64(call->parameters[1], len);
    bool size_const = try_eval_const_i64(call->parameters[3], size);
    bool use_plain = flag_const && flag == 0
	&& ((size_const && size < 0)
	 || (len_const && size_const && len >= 0 && size >= 0
	  && (uint64_t)len <= (uint64_t)size));

    std::string target_name = use_plain ? "snprintf" : "__snprintf_chk";
    std::vector<TokenBase *> args;
    args.push_back(call->parameters[0]);
    args.push_back(call->parameters[1]);
    if ( use_plain )
    {
	for ( size_t i = 4; i < call->parameters.size(); ++i )
	    args.push_back(call->parameters[i]);
    }
    else
    {
	args.push_back(call->parameters[2]);
	args.push_back(call->parameters[3]);
	for ( size_t i = 4; i < call->parameters.size(); ++i )
	    args.push_back(call->parameters[i]);
    }
    return redirect_builtin_call(pgm, target_name, args, regdp, call, storage);
}

//////////////////////////////////////////////////////////////////////////
//  Abs family handler							//
//////////////////////////////////////////////////////////////////////////

static Operand &emit_builtin_abs(Program &pgm, TokenCallFunc *call,
				 regdefp_t &regdp, Operand &storage)
{
    DataDef *builtin_type = direct_abs_builtin_type(call->var.name);
    FuncSignature funcsig(CallConvId::kCDecl);
    if ( direct_abs_builtin_returns_32bit(call->var.name) )
	funcsig.setRetT<int>();
    else
	funcsig.setRetT<int64_t>();
    add_funcsig_arg(funcsig, builtin_type);

    Operand arg_storage;
    DataDef *arg_type = NULL;
    Operand &arg = compile_call_arg_normalized(pgm, call->parameters[0], builtin_type, true, arg_storage, arg_type);
    std::vector<Operand> params;
    append_call_param(params, funcsig, arg, builtin_type);

    void *builtin_target = direct_abs_builtin_target(call->var.name);

    InvokeNode *invoke;
    pgm.cc.invoke(&invoke, imm(builtin_target), funcsig);
    pgm.track_invoke_target(builtin_target);
    set_invoke_args(pgm, invoke, params, false);

    if ( !regdp.first )
    {
	storage = pgm.cc.newGpq("builtin_abs_ret");
	regdp.first = &storage;
    }
    bind_call_return(pgm, invoke, regdp.first, builtin_type, storage,
		     /*is_variadic=*/false, false);
    regdp.second = builtin_type;
    return *regdp.first;
}

//////////////////////////////////////////////////////////////////////////
//  Dispatch table							//
//////////////////////////////////////////////////////////////////////////

struct BuiltinEntry
{
    const char *name;
    int min_argc;
    int max_argc;	// -1 = no upper bound (varargs)
    builtin_emit_fn handler;
};

static const BuiltinEntry builtin_table[] = {
    // Object size / frame address
    { "__builtin_object_size",		2,  2, emit_builtin_object_size },
    { "__builtin_frame_address",	1,  1, emit_builtin_frame_address },

    // Shuffle
    { "__builtin_shuffle",		2,  3, emit_builtin_shuffle },

    // Setjmp / longjmp / alloca
    { "__builtin_setjmp",		1,  1, emit_builtin_setjmp },
    { "__builtin_longjmp",		2,  2, emit_builtin_longjmp },
    { "__builtin_alloca",		1,  1, emit_builtin_alloca },
    { "alloca",				1,  1, emit_builtin_alloca },

    // Complex number functions
    { "__builtin_conj",			1,  1, emit_builtin_complex_conjugate_handler },
    { "__builtin_conjf",		1,  1, emit_builtin_complex_conjugate_handler },
    { "__builtin_conjl",		1,  1, emit_builtin_complex_conjugate_handler },
    { "conj",				1,  1, emit_builtin_complex_conjugate_handler },
    { "conjf",				1,  1, emit_builtin_complex_conjugate_handler },
    { "conjl",				1,  1, emit_builtin_complex_conjugate_handler },
    { "__builtin_creal",		1,  1, emit_builtin_creal_handler },
    { "__builtin_crealf",		1,  1, emit_builtin_creal_handler },
    { "__builtin_creall",		1,  1, emit_builtin_creal_handler },
    { "creal",				1,  1, emit_builtin_creal_handler },
    { "crealf",				1,  1, emit_builtin_creal_handler },
    { "creall",				1,  1, emit_builtin_creal_handler },
    { "__builtin_cimag",		1,  1, emit_builtin_cimag_handler },
    { "__builtin_cimagf",		1,  1, emit_builtin_cimag_handler },
    { "__builtin_cimagl",		1,  1, emit_builtin_cimag_handler },
    { "cimag",				1,  1, emit_builtin_cimag_handler },
    { "cimagf",				1,  1, emit_builtin_cimag_handler },
    { "cimagl",				1,  1, emit_builtin_cimag_handler },

    // _chk family — memory functions (4-arg: dst, src/val, len, size)
    { "__builtin___memset_chk",		4,  4, emit_chk_memset },
    { "__builtin___memcpy_chk",		4,  4, emit_chk_memcpy },
    { "__builtin___memmove_chk",	4,  4, emit_chk_memmove },
    { "__builtin___mempcpy_chk",	4,  4, emit_chk_mempcpy },
    { "__builtin___strncpy_chk",	4,  4, emit_chk_strncpy },
    { "__builtin___strncat_chk",	4,  4, emit_chk_strncat },
    { "__builtin___stpncpy_chk",	4,  4, emit_chk_stpncpy },

    // _chk family — string functions (3-arg: dst, src, size)
    { "__builtin___strcpy_chk",		3,  3, emit_chk_strcpy },
    { "__builtin___stpcpy_chk",		3,  3, emit_chk_stpcpy },
    { "__builtin___strcat_chk",		3,  3, emit_chk_strcat },

    // _chk family — printf variants
    { "__builtin___vsnprintf_chk",	6,  6, emit_chk_vsnprintf },
    { "__builtin___vsprintf_chk",	5,  5, emit_chk_vsprintf },
    { "__builtin___sprintf_chk",	4, -1, emit_chk_sprintf },
    { "__builtin___snprintf_chk",	5, -1, emit_chk_snprintf },

    { NULL, 0, 0, NULL }  // sentinel
};

// Build a hash map from the table on first use for O(1) lookup.
static const std::unordered_map<std::string, const BuiltinEntry *> &builtin_map()
{
    static std::unordered_map<std::string, const BuiltinEntry *> map;
    if ( map.empty() )
    {
	for ( const BuiltinEntry *e = builtin_table; e->name; ++e )
	    map[e->name] = e;
    }
    return map;
}

//////////////////////////////////////////////////////////////////////////
//  Entry point — called from TokenCallFunc::compile()			//
//////////////////////////////////////////////////////////////////////////

bool try_compile_builtin(Program &pgm, TokenCallFunc *call,
			 regdefp_t &regdp, Operand &storage,
			 Operand *&result)
{
    const std::string &name = call->var.name;

    // 1. Simple libc redirects (__builtin_printf → printf, etc.)
    if ( const char *target = simple_libc_builtin_redirect_target(name) )
    {
	Operand &r = redirect_builtin_call(pgm, target, call->parameters, regdp, call, storage);
	result = &r;
	return true;
    }

    // 2. Overflow predicate helpers (__madc_add_overflow_p, etc.)
    if ( is_overflow_predicate_helper_name(name) )
    {
	std::string target_name = typed_overflow_predicate_symbol_name(call->var, call->parameters);
	Operand &r = redirect_overflow_predicate_call(pgm, target_name, call->parameters, regdp, call, storage);
	result = &r;
	return true;
    }

    // 3. Overflow store helpers (__madc_add_overflow, etc.)
    if ( is_overflow_store_helper_name(name) )
    {
	DataDef *res_type = overflow_store_result_type(call->parameters);
	std::string target_name = typed_overflow_store_symbol_name(call->var, call->parameters);
	Operand &r = redirect_overflow_store_call(pgm, target_name, res_type, call->parameters, regdp, call, storage);
	result = &r;
	return true;
    }

    // 4. Dispatch table lookup
    const auto &map = builtin_map();
    auto it = map.find(name);
    if ( it != map.end() )
    {
	const BuiltinEntry *entry = it->second;
	int argc = (int)call->argc();
	if ( argc >= entry->min_argc && (entry->max_argc < 0 || argc <= entry->max_argc) )
	{
	    Operand &r = entry->handler(pgm, call, regdp, storage);
	    result = &r;
	    return true;
	}
    }

    // 5. Abs family — requires runtime disabled check
    if ( call->argc() == 1
      && is_direct_abs_builtin_name(name)
      && !direct_abs_builtin_disabled(pgm, name) )
    {
	Operand &r = emit_builtin_abs(pgm, call, regdp, storage);
	result = &r;
	return true;
    }

    return false;
}
