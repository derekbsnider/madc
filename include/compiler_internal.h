//////////////////////////////////////////////////////////////////////////
//									//
// Internal compiler helpers shared between compiler.cpp and		//
// compiler_builtins.cpp.  NOT part of the public API.			//
//									//
//////////////////////////////////////////////////////////////////////////
#ifndef __COMPILER_INTERNAL_H
#define __COMPILER_INTERNAL_H 1

#include <vector>
#include <string>
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "madc.h"
#include "madc_ir.h"

using namespace asmjit;

// --- Builtin dispatch entry point -------------------------------------------
//
// Called from TokenCallFunc::compile().  Returns true if the function was
// handled as a builtin, writing the result operand through *result.
// Returns false if the caller should fall through to normal call handling.

bool try_compile_builtin(Program &pgm, TokenCallFunc *call,
			 regdefp_t &regdp, Operand &storage,
			 Operand *&result);

// --- Shared compiler helpers ------------------------------------------------
//
// These live in compiler.cpp but are used by compiler_builtins.cpp as well.

bool try_eval_const_i64(TokenBase *tb, int64_t &out);
int64_t estimate_object_size(TokenBase *expr, int mode);
int64_t estimate_constant_printf_output(TokenBase *fmt_token,
					const std::vector<TokenBase *> &args,
					size_t arg_index);

Operand &emit_ir_value(Program &pgm, IRValue value, regdefp_t &regdp,
		       Operand &storage, DataDef *fallback_type);
IRValue ir_from_operand(const Operand &op, DataDef *type);

Operand &compile_call_arg_normalized(Program &pgm, TokenBase *token,
				     DataDef *target_type,
				     bool variadic_real_promotion,
				     Operand &storage, DataDef *&out_type);
void add_funcsig_arg(FuncSignature &funcsig, DataDef *arg_type);
void append_call_param(std::vector<Operand> &params, FuncSignature &funcsig,
		       const Operand &arg, DataDef *arg_type);
void set_invoke_args(Program &pgm, InvokeNode *call,
		     const std::vector<Operand> &params, bool mem_as_address);
void bind_call_return(Program &pgm, InvokeNode *call, Operand *dest,
		      DataDef *ret_type, Operand &fallback_operand,
		      bool is_variadic, bool narrow_int_ret = false);

// SIMD helpers
x86::Mem new_large_simd_stack(Program &pgm, DataDefSIMD *vdd, const char *name);
x86::Mem simd_lane_mem(const x86::Mem &base, size_t lane, DataDefSIMD *vdd);
x86::Gp load_simd_lane_gp(Program &pgm, const x86::Mem &base, size_t lane,
			   DataDefSIMD *vdd, const char *name);
void store_simd_lane_gp(Program &pgm, const x86::Mem &base, size_t lane,
			DataDefSIMD *vdd, x86::Gp value);
void store_xmm_to_mem(Program &pgm, x86::Mem &mem, x86::Xmm &xmm,
		       DataDef *type);
void load_mem_to_gpq(Program &pgm, x86::Gp &gp, const x86::Mem &mem,
		     DataDef *type);

// Type inference
DataDef *infer_numeric_type(TokenBase *left, TokenBase *right);

// Complex helpers
void prepare_complex_component_pair(Program &pgm, TokenBase *expr,
	DataDef *target_complex_type, Operand &base_storage,
	Operand *&real_part, DataDef *&real_type,
	Operand *&imag_part, DataDef *&imag_type,
	Operand &real_storage, Operand &imag_storage);
Operand &emit_complex_conjugate_expr(Program &pgm, TokenBase *arg_token,
	DataDef *complex_type, regdefp_t &regdp, Operand &storage);

// Redirect helper — builds a TokenCallFunc targeting a libc function
// and calls compile() on it.
Operand &redirect_builtin_call(Program &pgm, const std::string &target_name,
			       const std::vector<TokenBase *> &args,
			       regdefp_t &regdp, TokenCallFunc *site,
			       Operand &site_operand);

// Overflow helpers — called from both builtin dispatch and general call path
bool is_overflow_predicate_helper_name(const std::string &name);
bool is_overflow_store_helper_name(const std::string &name);
std::string typed_overflow_predicate_symbol_name(const Variable &var,
						 const std::vector<TokenBase *> &parameters);
DataDef *overflow_store_result_type(const std::vector<TokenBase *> &parameters);
std::string typed_overflow_store_symbol_name(const Variable &var,
					     const std::vector<TokenBase *> &parameters);

// --- AST walker ---------------------------------------------------------------
//
// Generic depth-first AST traversal.  Walks statement-level children of
// compound, control-flow, and switch/case nodes.  Calls pred(node) on
// every node; returns true on first match (short-circuits).
//
// Usage:
//   bool has_label = walk_ast(node, [](TokenBase *n) {
//       return dynamic_cast<TokenLabel *>(n) || dynamic_cast<TokenCASE *>(n);
//   });

template <typename Pred>
bool walk_ast(TokenBase *node, Pred pred)
{
    if ( !node ) return false;
    if ( pred(node) ) return true;

    if ( TokenCpnd *cpnd = dynamic_cast<TokenCpnd *>(node) )
    {
	for ( TokenStmt *s : cpnd->statements )
	    if ( walk_ast(s, pred) )
		return true;
	return false;
    }
    if ( TokenIF *tif = dynamic_cast<TokenIF *>(node) )
	return walk_ast(tif->statement, pred)
	    || walk_ast(tif->elsestmt, pred);
    if ( TokenFOR *tf = dynamic_cast<TokenFOR *>(node) )
	return walk_ast(tf->statement, pred);
    if ( TokenWHILE *tw = dynamic_cast<TokenWHILE *>(node) )
	return walk_ast(tw->statement, pred);
    if ( TokenDO *td = dynamic_cast<TokenDO *>(node) )
	return walk_ast(td->statement, pred);
    if ( TokenFOREACH *tfe = dynamic_cast<TokenFOREACH *>(node) )
	return walk_ast(tfe->statement, pred);
    if ( TokenSWITCH *tsw = dynamic_cast<TokenSWITCH *>(node) )
    {
	for ( TokenCASE *c : tsw->cases )
	    if ( walk_ast(c, pred) )
		return true;
	return walk_ast(tsw->defaultcase, pred);
    }
    if ( TokenCASE *tc = dynamic_cast<TokenCASE *>(node) )
    {
	for ( TokenBase *s : tc->statements )
	    if ( walk_ast(s, pred) )
		return true;
	return false;
    }
    return false;
}

// Runtime helpers (extern "C")
extern "C" void *__madc_jmpbuf_for(void *user_buf);
extern "C" void __madc_builtin_longjmp(void *user_buf, int value);
extern "C" long madc_builtin_imaxabs(long x);
extern "C" unsigned int madc_builtin_uabs(int x);
extern "C" unsigned long madc_builtin_ulabs(long x);
extern "C" unsigned long long madc_builtin_ullabs(long long x);
extern "C" unsigned long madc_builtin_umaxabs(long x);

#endif
