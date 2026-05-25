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
void load_mem_to_xmm(Program &pgm, x86::Xmm &xmm, const x86::Mem &mem,
		     DataDef *type);

// Type inference and type helpers
DataDef *infer_numeric_type(TokenBase *left, TokenBase *right);
DataDef *promote_c_integer_type(DataDef *type);
bool type_is_cstr_pointer(DataDef *dd);
bool is_large_struct_return(DataDef *ret_type);

// Operand helpers
x86::Gp as_gp_ptr(Program &pgm, Operand &op, const char *name = "mem_ptr");
void load_var_to_gp(Program &pgm, Operand &src, x86::Gp &dst_gp);
Operand &compile_token_normalized(Program &pgm, TokenBase *token,
				  DataDef *target_type, Operand *preferred_dest,
				  Operand &storage);
Operand &compile_token_gp_normalized(Program &pgm, TokenBase *token,
				     DataDef *target_type, Operand &storage);
Operand &compile_condition_operand(Program &pgm, TokenBase *condition,
				   Operand &storage);

// Emit helpers
void emit_raw_aggregate_copy(Program &pgm, Operand &dst, Operand &src,
			     DataDef *copy_type, const char *name);
void emit_small_struct_return(Program &pgm, Operand &src, DataDef *ret_type);
void emit_zeroed_void_return(Program &pgm);
void emit_function_instrument_exit(Program &pgm, FuncDef *current_func);

// Label helpers (defined in compiler_control_flow.cpp)
Label &lookup_or_make_label(Program &pgm, const std::string &name);

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

// Shared types (used by both compiler.cpp and compiler_operators.cpp)
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

// Operator helpers shared with compiler.cpp (defined in compiler_operators.cpp)
DataDef *token_numeric_type(TokenBase *token);
IntegerPrecision promoted_bitfield_precision(const DataDefSTRUCT::BitFieldInfo &bf);
IntegerPrecision binary_integer_precision(TokenBase *left, TokenBase *right,
					  DataDef *result_type);
bool is_arithmetic_result_operator(TokenBase *token);
DataDef *choose_integer_type(TokenBase *left, DataDef *lt,
			     TokenBase *right, DataDef *rt);
DataDef *usual_binary_integer_type(TokenBase *left, DataDef *lt,
				   TokenBase *right, DataDef *rt);
DataDef *effective_pointer_type_for_arith(Program &pgm, TokenBase *tb);
DataDef *complex_component_type(DataDef *dd);
void splat_xmm_to_simd_xmm(Program &pgm, x86::Xmm &dst, x86::Xmm src,
			    DataDefSIMD *vdd);
void splat_scalar_to_simd(Program &pgm, x86::Xmm &dst,
			  const IRValue &scalar, DataDefSIMD *vdd);
Operand &compile_complex_compare_base(Program &pgm, TokenBase *expr,
				      DataDef *expr_type, Operand &storage);
x86::Mem complex_component_mem(Program &pgm, const Operand &base_op,
			       DataDefCOMPLEX *cdd, bool imag,
			       const char *name);
x86::Mem large_simd_expr_mem(Program &pgm, TokenBase *expr,
			     DataDefSIMD *vdd, const char *name);
Operand &emit_complex_from_scalar(Program &pgm, Operand &scalar_op,
				  DataDef *scalar_type,
				  DataDef *complex_type, bool imag_value,
				  regdefp_t &regdp, Operand &storage,
				  const char *slot_name);

// SIMD / init helpers (defined in compiler.cpp)
bool is_large_simd_type(DataDef *type);
void emit_zero_fill_region(Program &pgm, x86::Gp &base_reg,
			   int32_t base_ofs, size_t total);
void emit_simd_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
		    DataDefSIMD *vdd, const std::vector<TokenBase *> &inits,
		    TokenBase *err_loc);
void load_idx_to_gpq(Program &pgm, x86::Gp &dst, Operand &src);

// Shared utility helpers (defined in compiler.cpp)
bool is_fixed_array_struct_member(TokenBase *tb);
bool subscript_object_uses_inplace_storage(const Variable &object);
DataDef *fixed_array_member_result_type(TokenMember *tm, size_t consumed_dims);
DataDef *fixed_array_subscript_result_type(const Variable &object,
					   size_t consumed_dims);
size_t fixed_array_subscript_stride(const Variable &object,
				    size_t consumed_dims);
x86::Xmm newScalarXmm(Program &pgm, DataDef *dd, const char *name);
void store_gp_to_var(Program &pgm, x86::Gp &src_gp, Operand &dst);
void lea_var_to_gp(Program &pgm, Operand &src, x86::Gp &dst_gp);
uint32_t scale_index_by_element_size(Program &pgm, x86::Gp &idx_reg,
				     DataDef *elem_type, const char *name);
x86::Gp materialize_gp_ptr_arg(Program &pgm, Operand &op, const char *name);
x86::Gp emit_runtime_struct_size(Program &pgm, DataDefSTRUCT *sdd,
				 const char *name);

bool token_has_constant_cstring(TokenBase *token);

// String helpers (defined in compiler.cpp / ns_common.cpp)
const char *string_cstr(void *ptr);
void string_assign(std::string &o, std::string &n);

// Runtime complex helpers (defined in compiler.cpp)
extern "C" void madc_runtime_complex_div_float(void *out,
    float ar, float ai, float br, float bi);
extern "C" void madc_runtime_complex_div_double(void *out,
    double ar, double ai, double br, double bi);

// Bitfield helpers (defined in compiler_operators.cpp)
x86::Gp emit_bitfield_load(Program &pgm, x86::Mem storage,
			   const DataDefSTRUCT::BitFieldInfo &bf,
			   const char *hint);
x86::Gp emit_bitfield_store_reg(Program &pgm, x86::Mem storage,
				const DataDefSTRUCT::BitFieldInfo &bf,
				x86::Gp value, const char *hint);
x86::Gp emit_bitfield_store_operand(Program &pgm, x86::Mem storage,
				    const DataDefSTRUCT::BitFieldInfo &bf,
				    Operand &value, DataDef *value_type,
				    DataDef *field_type, const char *hint);
x86::Gp emit_bitfield_load_storage(Program &pgm, x86::Mem storage,
				   const DataDefSTRUCT::BitFieldInfo &bf,
				   const char *hint);
void emit_bitfield_store_storage(Program &pgm, x86::Mem storage,
				const DataDefSTRUCT::BitFieldInfo &bf,
				x86::Gp merged);
void emit_bitfield_sign_extend(Program &pgm, x86::Gp &value,
			       const DataDefSTRUCT::BitFieldInfo &bf);

// Runtime helpers (extern "C")
extern "C" void *__madc_jmpbuf_for(void *user_buf);
extern "C" void __madc_builtin_longjmp(void *user_buf, int value);
extern "C" long madc_builtin_imaxabs(long x);
extern "C" unsigned int madc_builtin_uabs(int x);
extern "C" unsigned long madc_builtin_ulabs(long x);
extern "C" unsigned long long madc_builtin_ullabs(long long x);
extern "C" unsigned long madc_builtin_umaxabs(long x);

// Exception cleanup stack (exception_runtime.cpp)
extern "C" void __madc_cleanup_push(void *entry, void **fn_indirect,
				    void *obj, uint8_t *guard,
				    uint8_t is_chain_tail);
extern "C" void __madc_cleanup_pop();
extern "C" void __madc_cleanup_unwind_to(void *mark);
extern "C" void __madc_cleanup_discard_to(void *mark);

// Cleanup entry size for JIT stack allocation (must match MadcCleanupEntry)
static const uint32_t CLEANUP_ENTRY_SIZE = 40;
static const uint32_t CLEANUP_ENTRY_ALIGN = 8;

#endif
