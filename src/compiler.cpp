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
#include <map>
#include <set>
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
#include "madc_ir.h"

using namespace std;
using namespace asmjit;

static void bind_call_return(Program &pgm, InvokeNode *call, Operand *dest, DataDef *ret_type,
			     Operand &fallback_operand, bool is_variadic, bool narrow_int_ret=false);
const char *string_cstr(void *ptr);

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

static DataDef *infer_numeric_type(TokenBase *left, TokenBase *right)
{
    if ( (left && left->is_real()) || (right && right->is_real()) )
	return &ddDOUBLE;
    if ( left && left->datadef() && left->datadef()->is_integer() )
	return left->datadef();
    if ( right && right->datadef() && right->datadef()->is_integer() )
	return right->datadef();
    return &ddINT;
}

static Operand &compile_token_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
					 Operand *preferred_dest, Operand &storage)
{
    regdefp_t tmp_rdp = {preferred_dest, target_type, nullptr};
    Operand &raw = token->compile(pgm, tmp_rdp);
    DataDef *raw_type = tmp_rdp.second ? tmp_rdp.second : (token && token->datadef() ? token->datadef() : target_type);
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
typedef void (Program::*SafeBitOp2)(asmjit::Operand &, asmjit::Operand &);

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
    storage = lval;
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
    (pgm.*op)(lval, rval);
    storage = lval;
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
    storage = result;
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

static void emit_pointer_arith_scale(Program &pgm, TokenBase *left, TokenBase *right,
				     Operand &rval)
{
    DataDef *rtype = right ? right->datadef() : nullptr;
    if ( rtype && rtype->is_pointer() )
	return;  // ptr ± ptr: raw byte arithmetic, no scale
    size_t elem_size = 0;
    DataDef *ltype = left ? left->datadef() : nullptr;
    if ( ltype && ltype->is_pointer() )
    {
	DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(ltype);
	if ( pdd && pdd->base_type )
	    elem_size = pdd->base_type->size;
    }
    else if ( TokenVar *lvar = dynamic_cast<TokenVar *>(left) )
    {
	if ( lvar->var.is_fixed_array() && lvar->var.type )
	    elem_size = lvar->var.type->size;
    }
    if ( elem_size <= 1 )
	return;
    if ( !rval.isReg() || !rval.as<BaseReg>().isGroup(RegGroup::kGp) )
	return;
    pgm.cc.imul(rval.as<x86::Gp>(), rval.as<x86::Gp>(), imm((int64_t)elem_size));
}

enum class CmpKind : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

// Central implementation of `lval <cmp> rval` -> 0/1 int64 result.
// Both sides normalize through compile_token_normalized to a Reg of the
// inferred cmp_type; safecmp + the right safeset produce the flag and
// zero-extended byte; the final 0/1 routes through emit_ir_value so a
// caller's regdp.first (Reg or Mem) is honored uniformly.
static Operand &emit_compare(Program &pgm, TokenBase *left, TokenBase *right, CmpKind op,
			     regdefp_t &regdp, Operand &storage)
{
    Operand *caller_dest = regdp.first;
    DataDef *cmp_type = infer_numeric_type(left, right);
    // Real comparisons use ucomisd which sets CF/PF/ZF, not SF/OF,
    // so setl/setle/setg/setge are meaningless after ucomisd. The
    // x86 manual mandates the unsigned setcc variants (setb/setbe/
    // seta/setae) to read its flags correctly.
    bool is_unsigned = false;
    if ( op != CmpKind::Eq && op != CmpKind::Ne )
	is_unsigned = (left->datadef()  && left->datadef()->is_unsigned())
		   || (right->datadef() && right->datadef()->is_unsigned())
		   || (cmp_type && cmp_type->is_unsigned())
		   || (cmp_type && cmp_type->is_real());

    Operand left_norm;
    Operand right_norm;
    Operand &lval = compile_token_normalized(pgm, left, cmp_type, nullptr, left_norm);
    Operand &rval = compile_token_normalized(pgm, right, cmp_type, nullptr, right_norm);
    x86::Gp result = pgm.cc.newGpq("_cmp");
    pgm.safecmp(lval, rval);
    switch(op)
    {
	case CmpKind::Eq: pgm.safesete(result);                                          break;
	case CmpKind::Ne: pgm.safesetne(result);                                         break;
	case CmpKind::Lt: if ( is_unsigned ) pgm.safesetb(result);  else pgm.safesetl(result);  break;
	case CmpKind::Le: if ( is_unsigned ) pgm.safesetbe(result); else pgm.safesetle(result); break;
	case CmpKind::Gt: if ( is_unsigned ) pgm.safeseta(result);  else pgm.safesetg(result);  break;
	case CmpKind::Ge: if ( is_unsigned ) pgm.safesetae(result); else pgm.safesetge(result); break;
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

static Operand &compile_call_arg_normalized(Program &pgm, TokenBase *token, DataDef *target_type,
					    bool variadic_real_promotion,
					    Operand &storage, DataDef *&out_type)
{
    regdefp_t argrdp = {nullptr, nullptr, nullptr};
    if ( target_type && !(target_type->is_string() && token->datadef() && token->datadef()->is_pointer()) )
	argrdp.second = target_type;
    Operand &arg = token->compile(pgm, argrdp);
    DataDef *actual_type = argrdp.second ? argrdp.second : (token->datadef() ? token->datadef() : target_type);
    if ( !actual_type )
	throw "compile_call_arg_normalized() missing argument type";

    bool want_cstr = (target_type && target_type->type() == DataType::dtCHARptr
		      && token->datadef() && token->datadef()->rawtype() == DataType::dtSTRING)
		  || (variadic_real_promotion
		      && token->datadef() && token->datadef()->rawtype() == DataType::dtSTRING);
    if ( want_cstr )
    {
	x86::Gp cstr_reg = pgm.cc.newIntPtr("cstr");
	InvokeNode *cstr_call;
	pgm.cc.invoke(&cstr_call, imm(string_cstr), FuncSignature::build<const char *, void *>());
	cstr_call->setArg(0, arg.as<x86::Gp>());
	cstr_call->setRet(0, cstr_reg);
	storage = cstr_reg;
	out_type = &ddCHARptr;
	return storage;
    }

    DataDef *final_type = actual_type;
    if ( variadic_real_promotion && actual_type->is_real() )
	final_type = &ddDOUBLE;
    else if ( target_type && (target_type->is_numeric() || target_type->is_pointer()) )
	final_type = target_type;

    if ( (final_type->is_numeric() || final_type->is_pointer())
      && (arg.isReg() || arg.isMem() || arg.isImm()) )
    {
	IRBuilder ir(pgm.cc);
	IRValue out = ir.coerce(ir_from_operand(arg, actual_type), final_type);
	out = ir.load(out);
	storage = out.op;
	out_type = final_type;
	return storage;
    }

    storage = arg;
    out_type = actual_type;
    return storage;
}

static void add_funcsig_arg(FuncSignature &funcsig, DataDef *arg_type)
{
    if ( !arg_type )
    {
	funcsig.addArgT<void *>();
	return;
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
    if ( func->is_varargs ) expected_argc--;
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
	if ( !ptype->is_real() )
	    pgm.Throw(tn) << "Not expecting floating point argument" << flush;
	DBG(pgm.cc.comment("tnreg is Xmm"));
    }
    if ( tnreg.isReg() && tnreg.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	if ( ptype->is_real() )
	    pgm.Throw(tn) << "Expecting floating point argument" << flush;
	DBG(pgm.cc.comment("tnreg is Gp"));
	DBG(cout << "tnreg size=" << tnreg.x86RmSize() << " regdp.second->size="
		 << arg_type->size << " type " << arg_type->name << endl);
    }
    if ( tnreg.isImm() )
	pgm.cc.comment("tnreg is Imm");
}

static Operand &compile_condition_operand(Program &pgm, TokenBase *condition, Operand &storage)
{
    regdefp_t condrdp = {NULL, NULL, NULL};
    Operand &raw = condition->compile(pgm, condrdp);
    DataDef *cond_type = condrdp.second
	? condrdp.second
	: (condition && condition->datadef() ? condition->datadef() : &ddINT64);

    if ( cond_type && (cond_type->is_numeric() || cond_type->is_pointer())
      && (raw.isReg() || raw.isMem() || raw.isImm()) )
    {
	IRBuilder ir(pgm.cc);
	IRValue cond_value = ir.load(ir.coerce(ir_from_operand(raw, cond_type), cond_type));
	storage = cond_value.op;
	return storage;
    }

    if ( raw.isImm() )
    {
	x86::Gp cond_gp = pgm.cc.newGpq("__cond");
	pgm.cc.mov(cond_gp, raw.as<Imm>());
	storage = cond_gp;
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

void Program::_compiler_init()
{
    code.reset();
    code.init(jit.environment());
    DBG(
        static FileLogger logger(stdout);
        logger.setFlags(FormatFlags::kMachineCode);
        code.setLogger(&logger);
    );
//  this seems to break things at times
//  code.addEmitterOptions(BaseEmitter::kOptionStrictValidation);
    code.attach(&cc);
    // constant initialization
//  __const_double_1 = cc.newDoubleConst(ConstPool::kScopeGlobal, 1.0);
}

bool Program::_compiler_finalize()
{
    cc.ret(); // extra ret just in case
    asmjit::Error ferr = cc.finalize();
    DBG(if (ferr) std::cerr << "cc.finalize() error=" << ferr << std::endl);
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
	if ( !tf || !tf->var.data )
	    continue;

	Method *method = (Method *)tf->var.data;
	if ( !method || method->x86code )
	    continue;

	FuncDef *func = dynamic_cast<FuncDef *>(method->returns.type);
	if ( !func || !func->funcnode )
	    continue;

	method->x86code = (uint8_t *)root_fn + code.labelOffset(func->funcnode->label());
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

    // function pointer call — indirect invoke through address in variable
    if ( var.type->is_function() && var.type->is_numeric() )
    {
	DBG(cout << "TokenCallFunc::compile() function pointer call through " << var.name << endl);
	DBG(pgm.cc.comment("function pointer call"));

	DataDefFPTR *fptr = static_cast<DataDefFPTR *>(var.type);
	FuncDef *func = fptr->target;
	FuncSignature funcsig(CallConvId::kCDecl);
	bool ret_is_variadic = false;

	set_funcsig_ret(funcsig, &func->returns, func->is_multi_return());
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

	for ( size_t i = 0; i < argc(); ++i )
	{
	    // skip env param (index 0 in func->parameters) when capturing
	    size_t fi = func->has_captures ? i + 1 : i;
	    DataDef *ptype = fi < func->parameters.size() ? func->parameters[fi] : &ddINT64;
	    DataDef *arg_type = NULL;
	    Operand arg_storage;
	    Operand &areg = compile_call_arg_normalized(pgm, parameters[i], ptype, false, arg_storage, arg_type);
	    append_call_param(params, funcsig, areg, arg_type);
	}

	// invoke through register
	InvokeNode *call;
	pgm.cc.invoke(&call, ptr_op.as<x86::Gp>(), funcsig);
	set_invoke_args(pgm, call, params, true);

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

	// capture return value
	if ( retdd.rawtype() != DataType::dtVOID )
	{
	    if ( !regdp.first )
	    {
		if ( retdd.is_real() )
		    _operand = newScalarXmm(pgm, &retdd, "fptr_ret");
		else
		    _operand = pgm.cc.newGpq("fptr_ret");
		regdp.first = &_operand;
	    }
	    bind_call_return(pgm, call, regdp.first, &retdd, _operand, ret_is_variadic);
	}
	if ( !regdp.second )
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
	void *sym = dlsym(RTLD_DEFAULT, var.name.c_str());
	if ( sym )
	{
	    method->x86code = sym;
	    fnd = NULL; // intentional — fall into the typed-call path below
	    DBG(pgm.cc.comment("TokenCallFunc::compile() dlsym late-bind"));
	}
	else
	    pgm.Throw(this) << "TokenCallFunc::compile(" << var.name
			    << ") method has neither FuncNode nor x86code" << flush;
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

    if ( !regdp.second )
    {
	DBG(cout << "TokenCallFunc::compile(" << var.name << ") regdp.second = " << func->returns.name << endl);
	regdp.second = &func->returns;
    }

    DBG(cout << "TokenCallFunc::compile(" << var.name << ") func->returns.type() " << (int)func->returns.type() << endl);

    set_funcsig_ret(funcsig, &func->returns, func->is_multi_return());
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

    size_t expected_argc = explicit_expected_argc(func);
    if ( !is_variadic && !func->is_varargs && argc() > expected_argc )
	throw_too_many_call_args(pgm, this, parameters.data(), argc());

    size_t param_offset = (func->is_multi_return() || has_object_arg) ? 1 : 0;
    size_t fixed_argc = func->is_varargs ? expected_argc : argc();
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
	x86::Mem va_buf = pgm.cc.newStack((uint32_t)(nvarargs * 8), 8);
	x86::Gp va_ptr = pgm.cc.newIntPtr("__va_buf");
	pgm.cc.lea(va_ptr, va_buf);

	for ( size_t i = 0; i < nvarargs; ++i )
	{
	    TokenBase *tn_va = parameters[fixed_argc + i];
	    DataDef *va_type = NULL;
	    Operand va_storage;
	    Operand &va_reg = compile_call_arg_normalized(pgm, tn_va, NULL, true, va_storage, va_type);

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

	    x86::Mem slot = va_buf;
	    slot.addOffset((int64_t)(i * 8));
	    slot.setSize(8);
	    pgm.cc.mov(slot, val);
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
	void *sym = dlsym(RTLD_DEFAULT, var.name.c_str());
	if ( sym )
	{
	    call_target = sym;
	    DBG(cout << "TokenCallFunc::compile() redirecting " << var.name << " to C library version" << endl);
	}
    }

    // now we should have all we need to call the function
    DBG(pgm.cc.comment("pgm.call:"));
    DBG(pgm.cc.comment(var.name.c_str()));
    InvokeNode *call;
    DBG(cout << "invoke: argCount=" << funcsig.argCount() << " hasRet=" << funcsig.hasRet() << " is_variadic=" << is_variadic << endl);
    if ( fnd && !use_c_version ) pgm.cc.invoke(&call, fnd->label(), funcsig);
    else if ( is_variadic )
    {
	// variadic dlsym: load function pointer into Gp register for invoke
	x86::Gp fn_ptr = pgm.cc.newIntPtr("dl_fn");
	pgm.cc.mov(fn_ptr, imm(call_target));
	pgm.cc.invoke(&call, fn_ptr, funcsig);
    }
    else pgm.cc.invoke(&call, imm(call_target), funcsig);
    DBG(pgm.cc.comment("TokenCallFunc::compile() looping over params"));
    set_invoke_args(pgm, call, params, false);

    DBG(std::cout << "TokenCallFunc::compile() END" << std::endl);

    if ( !regdp.second )
	regdp.second = &func->returns;

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

Operand &TokenCpnd::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenCpnd::compile(" << (method ? method->returns.name : "") << ") TOP" << endl);
    for ( vector<TokenStmt *>::iterator vti = statements.begin(); vti != statements.end(); ++vti )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	(*vti)->compile(pgm, regdp);
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

    for ( vector<TokenStmt *>::iterator si = statements.begin(); si != statements.end(); ++si )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	(*si)->compile(pgm, regdp);
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
static void emit_struct_init(Program &pgm, x86::Gp &base_reg, int32_t base_ofs,
    DataDefSTRUCT *dds, const std::vector<TokenBase *> &inits, TokenBase *err_loc)
{
    size_t ofs = 0;
    for ( size_t i = 0; i < inits.size() && i < dds->members.size(); ++i )
    {
	auto &mp = dds->members[i];
	DataDef *mtype = mp.second;
	size_t fa = dds->field_align(*mtype);
	ofs = DataDefSTRUCT::align_up(ofs, fa);
	int32_t addr = base_ofs + (int32_t)ofs;

	// Nested struct-member init: `{ ..., { 0, 1, 10 }, ... }` where
	// the member is itself a struct value. TokenStructLit has no
	// compile() — recurse here instead of letting it fall through to
	// TokenStmt's default-throw branch.
	if ( mtype->basetype() == BaseType::btStruct
	  && dynamic_cast<TokenStructLit *>(inits[i]) != NULL )
	{
	    TokenStructLit *nested = static_cast<TokenStructLit *>(inits[i]);
	    DataDefSTRUCT *ndds = dynamic_cast<DataDefSTRUCT *>(mtype);
	    if ( !ndds )
		pgm.Throw(err_loc) << "Nested initializer for non-struct member type" << flush;
	    emit_struct_init(pgm, base_reg, addr, ndds, nested->inits, err_loc);
	    ofs += mtype->size;
	    continue;
	}

	// Nested fixed-array member init: `{ ..., { 0, 1, 10 }, ... }`
	// where the member is `sh_int liq_affect[3]` — a fixed array of
	// the element type. Per-element write at `[base + addr + j*esize]`.
	// Member's per-member count lives on the parent struct.
	{
	    std::string mname = mp.first;
	    size_t mcount = dds->m_count(mname);
	    TokenStructLit *nested_arr = dynamic_cast<TokenStructLit *>(inits[i]);
	    if ( mcount > 1 && nested_arr != NULL )
	    {
		size_t esize = mtype->size;
		for ( size_t j = 0; j < nested_arr->inits.size() && j < mcount; ++j )
		{
		    regdefp_t arr_rdp = {nullptr, nullptr, nullptr};
		    Operand &elem_val = nested_arr->inits[j]->compile(pgm, arr_rdp);
		    x86::Mem em = x86::ptr(base_reg, addr + (int32_t)(j * esize), (uint32_t)esize);
		    if ( elem_val.isImm() )
			pgm.cc.mov(em, elem_val.as<Imm>());
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
		for ( size_t j = nested_arr->inits.size(); j < mcount; ++j )
		{
		    x86::Mem em = x86::ptr(base_reg, addr + (int32_t)(j * esize), (uint32_t)esize);
		    pgm.cc.mov(em, imm(0));
		}
		ofs += esize * mcount;
		continue;
	    }
	}

	regdefp_t it_rdp = {nullptr, nullptr, nullptr};
	Operand &val_op = inits[i]->compile(pgm, it_rdp);

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
		cstr_call->setArg(0, val_op.as<x86::Gp>());
		cstr_call->setRet(0, cstr_gp);
		eff_val = cstr_gp;
	    }

	    size_t esize = mtype->size;
	    x86::Mem mm = x86::ptr(base_reg, addr, (uint32_t)esize);
	    if ( eff_val.isImm() )
		pgm.cc.mov(mm, eff_val.as<Imm>());
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
		call->setArg(1, val_op.as<x86::Gp>());
	    }
	}
	else
	{
	    pgm.Throw(err_loc) << "Unsupported struct member type in initializer: " << mtype->name << flush;
	}

	ofs += mtype->size;
    }
    // Zero-fill remaining numeric/pointer members
    for ( size_t i = inits.size(); i < dds->members.size(); ++i )
    {
	auto &mp = dds->members[i];
	DataDef *mtype = mp.second;
	size_t fa = dds->field_align(*mtype);
	ofs = DataDefSTRUCT::align_up(ofs, fa);
	if ( mtype->is_numeric() || mtype->is_pointer() )
	{
	    x86::Mem mm = x86::ptr(base_reg, base_ofs + (int32_t)ofs, (uint32_t)mtype->size);
	    pgm.cc.mov(mm, imm(0));
	}
	ofs += mtype->size;
    }
}

Operand &TokenDecl::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDecl::compile(" << var.name << " regdp.second: " << (regdp.second ? regdp.second->name : "")<<  ") TOP" << endl);

    // Top-level program declarations still need their initializer code emitted
    // so file-scope C globals like `static char *p = "x";` and static tables
    // are materialized before `main()` runs. Only local statics skip here.
    if ( initialize && (!(var.flags & vfSTATIC) || pgm.tkFunction == pgm.tkProgram) )
	initialize->compile(pgm, regdp);

    // brace-enclosed initializer for standalone structs: struct Foo x = { v0, v1, ... }
    if ( !init_list.empty() && !var.is_fixed_array()
      && dynamic_cast<DataDefSTRUCT *>(var.type) != NULL
      && var.type->type() == DataType::dtRESERVED )
    {
	DBG(pgm.cc.comment("TokenDecl struct init_list"));
	DataDefSTRUCT *dds = static_cast<DataDefSTRUCT *>(var.type);
	if ( init_list.size() > dds->members.size() )
	    pgm.Throw(this) << "Too many initializers for struct " << dds->name << flush;

	Operand &base_op = pgm.tkFunction->voperand(pgm, &var);
	x86::Gp base_reg = pgm.cc.newIntPtr("%s", (var.name + ".init_base").c_str());
	if ( base_op.isMem() )
	    pgm.cc.lea(base_reg, base_op.as<x86::Mem>());
	else
	    pgm.cc.mov(base_reg, base_op.as<x86::Gp>());

	emit_struct_init(pgm, base_reg, 0, dds, init_list, this);
    }

    // brace-enclosed initializer for fixed-size arrays: arr[N] = { v0, v1, ... }
    if ( !init_list.empty() && var.is_fixed_array() )
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
		if ( !slit )
		    pgm.Throw(this) << "Array-of-structs element must be a brace list { ... }" << flush;
		emit_struct_init(pgm, base_reg, (int32_t)(i * struct_size),
		    elem_dds, slit->inits, this);
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
	bool elem_is_charptr = var.type->is_pointer()
	    && var.type->rawtype() == DataType::dtCHAR;
	for ( size_t i = 0; i < flat_inits.size(); ++i )
	{
	    regdefp_t it_rdp = {nullptr, nullptr, nullptr};
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
		cstr_call->setArg(0, val_op.as<x86::Gp>());
		cstr_call->setRet(0, cstr);
		pgm.cc.mov(slot, cstr);
		continue;
	    }
	    if ( val_op.isImm() )
		pgm.cc.mov(slot, val_op.as<Imm>());
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

    set_funcsig_ret(funcsig, &func->returns, func->is_multi_return());

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
    if ( !var.data ) { throw "TokenFunc::compile: method is NULL"; }

    Method &method = *((Method *)var.data);
    FuncDef *func = (FuncDef *)method.returns.type;

    // Pre-pass in Program::compile usually already prepared this function's
    // funcnode so global fn-pointer inits could LEA its label. If not
    // (e.g. a late-compiled lambda), do it now.
    prepareFuncNode(pgm);

    pgm.tkFunction = this;
    clear_operand_map(); // clear operand map
    pgm.label_map.clear(); // labels are function-scoped

    pgm.cc.addFunc(func->funcnode);

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
		    IRBuilder ir(pgm.cc);
		    ir.store(IRValue::mem(reg.as<x86::Mem>(), (*vvi)->type),
			     ir_from_operand(tmp, (*vvi)->type));
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

    if ( variables.size() )
    {
	DBG(cout << "Local variables:" << endl);
	for ( variable_vec_iter vvi = variables.begin(); vvi != variables.end(); ++vvi )
	{
	    DBG(cout << "    " << (*vvi)->type->name << ' ' << (*vvi)->name << endl);
	}
    }

    for ( vector<TokenStmt *>::iterator si = statements.begin(); si != statements.end(); ++si )
    {
	// each new statement starts with a clean slate
	regdp = {NULL, NULL, NULL};
	(*si)->compile(pgm, regdp);
    }

    cleanup(pgm);	// cleanup stack
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
    _compiler_init();

    // Pre-pass: create FuncNode labels for every user-defined function and
    // lambda so global fn-pointer inits (which compile first via TokenProgram)
    // can LEA the target label. The bodies still compile in the normal loop.
    try
    {
	for ( TokenBase *tb_func : pending_funcs )
	{
	    prepass_tb = tb_func;
	    TokenFunc *tf = dynamic_cast<TokenFunc *>(tb_func);
	    if ( tf ) tf->prepareFuncNode(*this);
	}
    }
    catch(const char *err_msg)
    {
	cerr << ANSI_WHITE;
	if ( prepass_tb && prepass_tb->file )
	    cerr << prepass_tb->file << ':' << prepass_tb->line << ':' << prepass_tb->column;
	else
	    cerr << ':';
	cerr << ": \e[1;31merror:\e[1;37m "
	     << (err_msg ? err_msg : "(null error message)")
	     << " (during funcnode pre-pass)"
	     << ANSI_RESET << endl;
	if ( prepass_tb )
	    source.showerror(prepass_tb->line, prepass_tb->column);
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
	cerr << ANSI_WHITE;
	if ( tb && tb->file )
	    cerr << tb->file << ':' << tb->line << ':' << tb->column;
	else
	    cerr << ':';
	cerr << ": \e[1;31merror:\e[1;37m "
	     << (err_msg ? err_msg : "(null error message)")
	     << ANSI_RESET << endl;
	if ( tb )
	    source.showerror(tb->line, tb->column);
	return false;
    }
    catch(TokenBase *tb)
    {
	cerr << ANSI_WHITE << (tb->file ? tb->file : "NULL") << ':' << tb->line << ':' << tb->column
	     << ": \e[1;31merror:\e[1;37m unexpected token type " << (int)tb->type() << " value " << (int)tb->get() << " char " << (char)tb->get() << ANSI_RESET << endl;
	source.showerror(tb->line, tb->column);
	return false;
    }
    catch(std::exception &e)
    {
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

    DBG(std::cout << "Program::execute() calling root_fn()" << std::endl);
    root_fn();

    if ( !var )
    {
	DBG(std::cerr << "Program::execute() cannot find main" << std::endl);
	return;
    }
    if ( var->type->basetype() != BaseType::btFunct )
    {
	std::cerr << "Program::execute() main is not a function" << std::endl;
	return;
    }
    if ( !(method=(Method *)var->data) )
    {
	std::cerr << "Program::execute() main method is NULL" << std::endl;
	return;
    }
    if ( !(main_fn=(fVOIDFUNC)method->x86code) )
    {
	std::cerr << "Program::execute() main has no x86 code" << std::endl;
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

    DBG(std::cout << std::endl << "Program::execute() main() returns" << std::endl);
    DBG(std::cout << "Program::execute() ends" << std::endl);
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

    void finish(Program &pgm)
    {
	if ( tv )
	{
	    tv->var.modified();
	    tv->putreg(pgm);
	}
	if ( is_member && writeback.hasBase() )
	{
	    IRBuilder ir(pgm.cc);
	    ir.store(IRValue::mem(writeback, type), ir_from_operand(lval, type));
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

static CompoundLHS resolveCompoundLHS(Program &pgm, TokenBase *left, const char *op_name)
{
    CompoundLHS r;
    r.tv = NULL;
    r.is_member = false;
    r.type = NULL;

    if ( left->type() == TokenType::ttVariable )
    {
	r.tv = dynamic_cast<TokenVar *>(left);
	r.type = r.tv->var.type;
	Operand &op = r.tv->operand(pgm);
	if ( op.isMem() )
	{
	    if ( r.type && r.type->is_real() )
	    {
		// Double/float lval on stack — load into Xmm so the
		// subsequent safeadd / safemul / etc. all work with
		// matching-type operands.
		IRBuilder ir(pgm.cc);
		IRValue lhs = ir.load(IRValue::mem(op.as<x86::Mem>(), r.type));
		r.writeback = op.as<x86::Mem>();
		r.lval = lhs.op;
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
    else if ( left->type() == TokenType::ttMember )
    {
	// struct member via . or -> : load Mem into temp register
	TokenMember *tm = dynamic_cast<TokenMember *>(left);
	if ( tm )
	{
	    r.type = tm->var.type;
	    Operand &mem = tm->operand(pgm);
	    if ( mem.isMem() )
	    {
		if ( r.type && r.type->is_real() )
		{
		    // Double/float struct member — load into Xmm so the
		    // subsequent safeadd / safemul / etc. use Xmm/Xmm paths.
		    IRBuilder ir(pgm.cc);
		    IRValue lhs = ir.load(IRValue::mem(mem.as<x86::Mem>(), r.type));
		    r.writeback = mem.as<x86::Mem>();
		    r.lval = lhs.op;
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
	    if ( td )
	    {
		r.type = td->deref_type;
		Operand &mem = td->operand(pgm);
		if ( mem.isMem() )
		{
		    if ( r.type && r.type->is_real() )
		    {
			IRBuilder ir(pgm.cc);
			IRValue lhs = ir.load(IRValue::mem(mem.as<x86::Mem>(), r.type));
			r.writeback = mem.as<x86::Mem>();
			r.lval = lhs.op;
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
	    if ( idx_op.isReg() )
		pgm.cc.mov(idx_reg, idx_op.as<x86::Gp>());
	    else
		pgm.cc.mov(idx_reg, idx_op.as<Imm>());
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
	    if ( idx_op.isReg() )
		pgm.cc.mov(idx_reg, idx_op.as<x86::Gp>());
	    else if ( idx_op.isImm() )
		pgm.cc.mov(idx_reg, idx_op.as<Imm>());
	    else if ( idx_op.isMem() )
		pgm.cc.mov(idx_reg, idx_op.as<x86::Mem>());

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
	    if ( idx_op.isReg() )
		pgm.cc.mov(idx_reg, idx_op.as<x86::Gp>());
	    else if ( idx_op.isImm() )
		pgm.cc.mov(idx_reg, idx_op.as<Imm>());
	    else if ( idx_op.isMem() )
		pgm.cc.mov(idx_reg, idx_op.as<x86::Mem>());

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
		(pgm.*step)(reg);
	    }
	    else
	    {
		(pgm.*step)(reg);
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
	    (pgm.*step)(reg);
	}
	else
	{
	    (pgm.*step)(reg);
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
    (pgm.*step)(lhs.lval);
    if ( lhs.is_member && lhs.writeback.hasBase() )
    {
	IRBuilder ir(pgm.cc);
	ir.store(IRValue::mem(lhs.writeback, lhs.type), ir_from_operand(lhs.lval, lhs.type));
    }
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
    CompoundLHS lhs = resolveCompoundLHS(pgm, left, op_name);
    Operand tmp;
    Operand *out = regdp.first;
    regdp.second = lhs.type;
    regdp.first  = nullptr;
    Operand &rval = compile_compound_rhs_normalized(pgm, right, lhs.type, tmp);
    (pgm.*op)(lhs.lval, rval, lhs.type, nullptr);
    return finish_compound_assign(pgm, regdp, lhs, _operand, out);
}

// Helper for |=, &=, ^=, <<=, >>= (2-arg bit/shift safe ops; right side is
// forced to a Gp).
static Operand &emit_compound_bitop2(Program &pgm, TokenBase *left, TokenBase *right,
				     SafeBitOp2 op, regdefp_t &regdp, Operand &_operand,
				     const char *op_name)
{
    CompoundLHS lhs = resolveCompoundLHS(pgm, left, op_name);
    Operand tmp;
    Operand *out = regdp.first;
    regdp.second = lhs.type;
    regdp.first  = nullptr;
    Operand &rval = compile_compound_rhs_gp_normalized(pgm, right, lhs.type, tmp);
    (pgm.*op)(lhs.lval, rval);
    return finish_compound_assign(pgm, regdp, lhs, _operand, out);
}

// Helper for /= (return_remainder=false) and %= (return_remainder=true).
// safediv needs three distinct Gp regs, so the LHS value gets copied into
// a fresh dividend Reg and the result is stored back to lhs.lval.
static Operand &emit_compound_divmod(Program &pgm, TokenBase *left, TokenBase *right,
				     bool return_remainder, regdefp_t &regdp,
				     Operand &_operand, const char *op_name)
{
    CompoundLHS lhs  = resolveCompoundLHS(pgm, left, op_name);
    Operand dividend  = lhs.type->newreg(pgm.cc, "dividend");
    Operand remainder = lhs.type->newreg(pgm.cc, "remainder");
    Operand divisor;
    Operand *out = regdp.first;
    pgm.safemov(dividend, lhs.lval);
    regdp.second = lhs.type;
    regdp.first  = nullptr;
    compile_compound_rhs_normalized(pgm, right, lhs.type, divisor);
    if ( lhs.type->is_integer() )
	pgm.safexor(remainder, remainder);
    pgm.safediv(remainder, dividend, divisor, lhs.type);
    pgm.safemov(lhs.lval, return_remainder ? remainder : dividend);
    return finish_compound_assign(pgm, regdp, lhs, _operand, out);
}

Operand &TokenAddEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "+= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "+= missing rval operand" << flush;
    return emit_compound_binop3(pgm, left, right, &Program::safeadd, regdp, _operand, "+=");
}

Operand &TokenSubEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "-= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "-= missing rval operand" << flush;
    return emit_compound_binop3(pgm, left, right, &Program::safesub, regdp, _operand, "-=");
}

Operand &TokenMulEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "*= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "*= missing rval operand" << flush;
    return emit_compound_binop3(pgm, left, right, &Program::safemul, regdp, _operand, "*=");
}

Operand &TokenDivEq::compile(Program &pgm, regdefp_t &regdp)
{
    if ( !left )  pgm.Throw(this) << "/= missing lval operand" << flush;
    if ( !right ) pgm.Throw(this) << "/= missing rval operand" << flush;
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
		scall->setArg(0, var_op.as<x86::Gp>());
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
	    if ( elem_type && elem_type->is_pointer()
	      && elem_type->rawtype() == DataType::dtCHAR
	      && right->datadef()
	      && right->datadef()->rawtype() == DataType::dtSTRING )
	    {
		x86::Gp cstr = pgm.cc.newIntPtr("subx_cstr");
		InvokeNode *cstr_call;
		pgm.cc.invoke(&cstr_call, imm(string_cstr),
		    FuncSignature::build<const char *, void *>());
		cstr_call->setArg(0, rhs_op.as<x86::Gp>());
		cstr_call->setRet(0, cstr);
		effective_rhs = cstr;
	    }

	    // Use operand() rather than compile() here: for an aggregate
	    // base_expr (e.g. a struct-contained `int bits[4]` exposed as
	    // TokenMember with a numeric _datatype), compile() would emit
	    // the load-first-element-into-Gp path via emit_ir_value, and we'd
	    // end up indexing off the element value instead of the array
	    // base address. operand() returns the raw Mem/Gp.
	    Operand &base_op = tse->base_expr->operand(pgm);
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
	    if ( idx_op.isReg() && idx_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.mov(idx_reg, idx_op.as<x86::Gp>());
	    else if ( idx_op.isImm() )
		pgm.cc.mov(idx_reg, idx_op.as<Imm>());
	    else if ( idx_op.isMem() )
		pgm.cc.mov(idx_reg, idx_op.as<x86::Mem>());

	    // SIB scale only covers 1/2/4/8 — for any other element size (e.g.
	    // sizeof(struct K) == 16) fold the element stride into the index
	    // via imul and access with scale = 1.
	    uint32_t shift = 0;
	    if      ( elem_size == 8 ) shift = 3;
	    else if ( elem_size == 4 ) shift = 2;
	    else if ( elem_size == 2 ) shift = 1;
	    else if ( elem_size != 1 )
		pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)elem_size));
	    x86::Mem slot = x86::ptr(base_reg, idx_reg, shift, 0, (uint32_t)elem_size);

	    if ( effective_rhs.isReg() && effective_rhs.as<BaseReg>().isGroup(RegGroup::kGp) )
	    {
		IRBuilder ir(pgm.cc);
		ir.store(IRValue::mem(slot, elem_type),
			 ir_from_operand(effective_rhs.as<x86::Gp>(), elem_type));
	    }
	    else if ( effective_rhs.isImm() )
		pgm.cc.mov(slot, effective_rhs.as<Imm>());
	    else
		throw "TokenAssign: unsupported RHS for subscript store";

	    _operand = effective_rhs;
	    regdp.first = &_operand;
	    regdp.second = elem_type;
	    return _operand;
	}
	ltype = tsub->datadef();
	DBG(cout << "TokenAssign::compile() subscript assignment to " << tsub->object.name << " elem type " << ltype->name << endl);
	DBG(pgm.cc.comment("TokenAssign: subscript write"));
	// compile right side independently
	regdefp_t rhs_rdp = {nullptr, nullptr, nullptr};
	rhs_rdp.second = ltype;
	Operand &rhs_op = right->compile(pgm, rhs_rdp);
	// Coerce dtSTRING → char*: storing a string literal into a
	// `char *arr[N]` slot used to write the std::string object's
	// address into the slot (not its c_str()). Match the same
	// coercion TokenAssign applies for plain `char *p = "literal";`.
	Operand effective_rhs = rhs_op;
	if ( ltype->is_pointer() && ltype->rawtype() == DataType::dtCHAR
	  && right->datadef() && right->datadef()->rawtype() == DataType::dtSTRING )
	{
	    x86::Gp cstr = pgm.cc.newIntPtr("sub_cstr");
	    InvokeNode *cstr_call;
	    pgm.cc.invoke(&cstr_call, imm(string_cstr),
		FuncSignature::build<const char *, void *>());
	    cstr_call->setArg(0, rhs_op.as<x86::Gp>());
	    cstr_call->setRet(0, cstr);
	    effective_rhs = cstr;
	}
	tsub->compile_set(pgm, effective_rhs, rhs_rdp.second ? rhs_rdp.second : ltype);
	_operand = effective_rhs;
	regdp.first = &_operand;
	regdp.second = ltype;
	return _operand;
    }
    else
    {
	pgm.Throw(this) << "Assignment on a non-variable lval" << flush;
    }

    if ( !regdp.first || !regdp.second )
	regdp.second = ltype; // set type if not set

    // Post-declaration string-literal → char* assignment: `char *p; p =
    // "literal";`. The LHS is a char* (pointer, so is_numeric), but the
    // RHS is a dtSTRING object — passing the dtSTRING operand as
    // regdp.first would write the std::string pointer into p instead of
    // its data. Compile the RHS into a tmp, pass it through string_cstr,
    // then store the resulting char* into p's register.
    if ( ltype->is_pointer() && ltype->rawtype() == DataType::dtCHAR
      && right->datadef() && right->datadef()->rawtype() == DataType::dtSTRING )
    {
	regdefp_t rhs_rdp = {NULL, NULL, NULL};
	Operand &str_op = right->compile(pgm, rhs_rdp);
	x86::Gp cstr = pgm.cc.newIntPtr("cstr");
	InvokeNode *call;
	pgm.cc.invoke(&call, imm(string_cstr), FuncSignature::build<const char *, void *>());
	call->setArg(0, str_op.as<x86::Gp>());
	call->setRet(0, cstr);
	if ( _operand.isReg() && _operand.as<BaseReg>().isGroup(RegGroup::kGp) )
	    pgm.cc.mov(_operand.as<x86::Gp>(), cstr);
	else if ( _operand.isMem() )
	    pgm.cc.mov(_operand.as<x86::Mem>(), cstr);
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
	r_operand = &right->compile(pgm, regdp);
	regdp.first = orig_operand ? orig_operand : r_operand;
    }

    if ( !regdp.second )
	throw "TokenAssign: no rval type";
    if (  ltype->is_numeric() && !regdp.second->is_numeric() )
	throw "Expecting rval to be numeric";
    if ( !ltype->is_integer() &&  regdp.second->is_integer() )
	throw "Not expecting rval to be numeric";
    if ( ltype->is_string() && !regdp.second->is_string() )
	throw "Expecting rval to be string";

    if ( ltype->is_numeric() )
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
        InvokeNode* call; pgm.cc.invoke(&call, imm(string_assign), FuncSignature::build<void, const char*, const char *>());
	call->setArg(0, _operand.as<x86::Gp>());
	call->setArg(1, r_operand->as<x86::Gp>());
	if ( tvl )
	{
	    tvl->var.modified();
	    tvl->putreg(pgm);
	}
    }
    else
    if ( ltype->basetype() == BaseType::btStruct
      || ltype->basetype() == BaseType::btClass )
    {
	// struct-to-struct copy: emit memcpy(&dest, &src, sizeof(S)).
	// Both sides expose their struct storage either as a Mem (stack
	// slot for a local struct variable) or as a Gp holding a LEA'd
	// address (e.g. `obj->member` through TokenMember::operand for a
	// non-numeric member). LEA the Mem form into a Gp; use the Gp
	// form directly.
	if ( ltype != regdp.second )
	    throw "TokenAssign struct: lhs/rhs struct types differ";
	DBG(cout << "TokenAssign::compile() struct to struct (" << ltype->name << ", " << ltype->size << " bytes)" << endl);
	DBG(pgm.cc.comment("TokenAssign::compile() struct memcpy"));
	x86::Gp dst_gp = pgm.cc.newIntPtr("struct_copy_dst");
	if ( _operand.isMem() )
	    pgm.cc.lea(dst_gp, _operand.as<x86::Mem>());
	else if ( _operand.isReg() && _operand.as<BaseReg>().isGroup(RegGroup::kGp) )
	    pgm.cc.mov(dst_gp, _operand.as<x86::Gp>());
	else
	    throw "TokenAssign struct: unsupported LHS operand shape";
	x86::Gp src_gp = pgm.cc.newIntPtr("struct_copy_src");
	if ( r_operand->isMem() )
	    pgm.cc.lea(src_gp, r_operand->as<x86::Mem>());
	else if ( r_operand->isReg() && r_operand->as<BaseReg>().isGroup(RegGroup::kGp) )
	    pgm.cc.mov(src_gp, r_operand->as<x86::Gp>());
	else
	    throw "TokenAssign struct: unsupported RHS operand shape";
	InvokeNode *mcall;
	pgm.cc.invoke(&mcall, imm((void *)memcpy),
	    FuncSignature::build<void *, void *, void *, size_t>());
	mcall->setArg(0, dst_gp);
	mcall->setArg(1, src_gp);
	mcall->setArg(2, imm((size_t)ltype->size));
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
    pgm.tkFunction->putreg(pgm.cc, &var);
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

	if ( object.type->is_pointer() )
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

	    if ( var.type->is_numeric() )
		_operand = x86::ptr(obj_gp, (int32_t)offset, (uint32_t)var.type->size);
	    else
	    {
		x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
		DBG(pgm.cc.comment("TokenMember::operand() chained -> lea non-numeric member"));
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
		if ( var.type->is_numeric() )
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

		if ( var.type->is_numeric() )
		    _operand = member_mem;
		else
		{
		    x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
		    DBG(pgm.cc.comment("TokenMember::operand() chained . lea non-numeric member"));
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
	if ( var.type->is_numeric() )
	    _operand = x86::ptr(obj_gp, (int32_t)offset, (uint32_t)var.type->size);
	else
	{
	    x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
	    DBG(pgm.cc.comment("TokenMember::operand() lea from stack-backed pointer"));
	    pgm.cc.lea(addr_reg, x86::ptr(obj_gp, (int32_t)offset));
	    _operand = addr_reg;
	}
	return _operand;
    }

    if ( _obj.isMem() )
    {
	// Struct/array on the JIT stack: compute [struct_base + member_offset]
	x86::Mem member_mem = _obj.as<x86::Mem>();
	member_mem.setSize(var.type->size);
	member_mem.addOffset((int64_t)offset);

	if ( var.type->is_numeric() )
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
	if ( var.type->is_numeric() )
	{
	    x86::Mem member_mem = x86::ptr(obj_gp, (int32_t)offset, (uint32_t)var.type->size);
	    _operand = member_mem;
	}
	else
	{
	    x86::Gp addr_reg = pgm.cc.newIntPtr("%s", var.name.c_str());
	    DBG(pgm.cc.comment("TokenMember::operand() lea from pointer base"));
	    pgm.cc.lea(addr_reg, x86::ptr(obj_gp, (int32_t)offset));
	    _operand = addr_reg;
	}
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
    pgm.tkFunction->putreg(pgm.cc, &var);
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
	    pgm.cc.mov(dest->as<x86::Gp>(), ret_gp);
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
	  || (ret_type->is_real() && !fallback_operand.as<BaseReg>().isGroup(RegGroup::kVec))
	  || (!ret_type->is_real() && !fallback_operand.as<BaseReg>().isGroup(RegGroup::kGp)) )
	    fallback_operand = ret_type->newreg(pgm.cc, "call_ret");
	if ( ret_type->is_real() )
	    call->setRet(0, fallback_operand.as<x86::Xmm>());
	else
	    call->setRet(0, fallback_operand.as<x86::Gp>());
	return;
    }

    if ( dest->isReg() )
    {
	if ( dest->as<BaseReg>().isGroup(RegGroup::kVec) )
	{
	    if ( is_variadic )
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
	if ( ret_type->is_real() )
	{
	    x86::Xmm ret_xmm = newScalarXmm(pgm, ret_type, "call_ret");
	    call->setRet(0, ret_xmm);
	    IRBuilder ir(pgm.cc);
	    ir.store(IRValue::mem(dest->as<x86::Mem>(), ret_type), ir_from_operand(ret_xmm, ret_type));
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

void TokenCpnd::movreg(x86::Compiler &cc, Operand &op, Variable *var)
{
    if ( !op.isReg() )
    {
	DBG(cc.comment("TokenCpnd::movreg() operand is not a register"));
	return;
    }
    if ( op.as<BaseReg>().isGroup(RegGroup::kVec) )
    {
	DBG(cc.comment("TokenCpnd::movreg() calling movmptr2xval(cc, reg, var->data)"));
	DBG(cc.comment(var->name.c_str()));
	var->type->movmptr2xval(cc, op.as<x86::Xmm>(), var->data);
    }
    if ( op.as<BaseReg>().isGroup(RegGroup::kGp) )
    {
	DBG(cc.comment("TokenCpnd::movreg() calling movmptr2rval(cc, reg, var->data)"));
	DBG(cc.comment(var->name.c_str()));
	var->type->movmptr2rval(cc, op.as<x86::Gp>(), var->data);
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

    // get container pointer
    Operand &obj_op = pgm.tkFunction->voperand(pgm, &object);
    x86::Gp obj_reg = pgm.cc.newIntPtr("sub_obj");
    pgm.cc.mov(obj_reg, obj_op.as<x86::Gp>());

    // compile index expression
    regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
    Operand &idx_op = index->compile(pgm, idx_rdp);
    x86::Gp idx_reg = pgm.cc.newGpq("sub_idx");
    if ( idx_op.isReg() )
        pgm.cc.mov(idx_reg, idx_op.as<x86::Gp>());
    else
        pgm.cc.mov(idx_reg, idx_op.as<Imm>());

    DataType ctype = object.type->type();

    // C fixed-size array: load element directly from [base + linear_idx*elem_size]
    if ( object.is_fixed_array() )
    {
        size_t elem_size = object.type->size ? object.type->size : 8;
        // Fold any extra indices (multi-dim): linear = ((i0*d1) + i1)*d2 + i2 ...
        for ( size_t k = 0; k < extra_indices.size(); ++k )
        {
            uint32_t dim_k = object.dims[k + 1];
            pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)dim_k));
            regdefp_t ex_rdp = {nullptr, nullptr, nullptr};
            Operand &ex_op = extra_indices[k]->compile(pgm, ex_rdp);
            if ( ex_op.isReg() )
                pgm.cc.add(idx_reg, ex_op.as<x86::Gp>());
            else
                pgm.cc.add(idx_reg, ex_op.as<Imm>());
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

        uint32_t shift = 0;
        if      ( elem_size == 8 ) shift = 3;
        else if ( elem_size == 4 ) shift = 2;
        else if ( elem_size == 2 ) shift = 1;
        DBG(pgm.cc.comment("fixed-array subscript read"));
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
        if ( !regdp.second )
            regdp.second = _datatype;
        return emit_ir_value(pgm, IRValue::mem(elem_mem, _datatype), regdp, _operand, _datatype);
    }

    // Raw pointer subscript: ptr[i] == *(ptr + i).
    // obj_reg already holds the pointer value (from voperand). Compute
    // [ptr + idx * sizeof(base_type)] with SIB scale when possible.
    if ( !object.is_fixed_array() && object.type->is_pointer() )
    {
        DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(object.type);
        DataDef *base = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
        size_t elem_size = base->size ? base->size : 8;
        uint32_t shift = 0;
        if      ( elem_size == 8 ) shift = 3;
        else if ( elem_size == 4 ) shift = 2;
        else if ( elem_size == 2 ) shift = 1;
        DBG(pgm.cc.comment("pointer subscript read"));
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
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
     || dynamic_cast<TokenCast *>(base_expr) != NULL;
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
    else if ( base_op.isReg() && base_op.as<BaseReg>().isGroup(RegGroup::kGp) )
	pgm.cc.mov(base_reg, base_op.as<x86::Gp>());
    else
	pgm.Throw(this) << "TokenSubscriptExpr::compile() unsupported base operand" << flush;

    regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
    Operand &idx_op = index->compile(pgm, idx_rdp);
    x86::Gp idx_reg = pgm.cc.newGpq("subexpr_idx");
    if ( idx_op.isReg() )
	pgm.cc.mov(idx_reg, idx_op.as<x86::Gp>());
    else
	pgm.cc.mov(idx_reg, idx_op.as<Imm>());

    size_t elem_size = _datatype && _datatype->size ? _datatype->size : 8;
    // SIB scale only covers 1/2/4/8; any other element size (notably struct
    // elements like sizeof(struct K) == 16) must be folded into the index
    // register via imul, then accessed with SIB scale = 1.
    uint32_t shift = 0;
    if      ( elem_size == 8 ) shift = 3;
    else if ( elem_size == 4 ) shift = 2;
    else if ( elem_size == 2 ) shift = 1;
    else if ( elem_size != 1 )
    {
	pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)elem_size));
    }

    x86::Mem elem_mem = x86::ptr(base_reg, idx_reg, shift, 0, (uint32_t)elem_size);
    if ( !regdp.second )
	regdp.second = _datatype;
    // Struct/class element: return the Mem directly. emit_ir_value assumes
    // a numeric-sized value it can coerce/load; a struct element isn't a
    // value that fits in a register. Callers (e.g. TokenMember::operand
    // for `s[i].member`) read the Mem and add the member offset.
    if ( _datatype && (_datatype->basetype() == BaseType::btStruct
		    || _datatype->basetype() == BaseType::btClass) )
    {
	_operand = elem_mem;
	if ( !regdp.first )
	    regdp.first = &_operand;
	return _operand;
    }
    return emit_ir_value(pgm, IRValue::mem(elem_mem, _datatype), regdp, _operand, _datatype);
}

// Write subscript: container[index] = val  (called from TokenAssign::compile)
void TokenSubscript::compile_set(Program &pgm, Operand &val_op, DataDef *val_type)
{
    DBG(pgm.cc.comment("TokenSubscript::compile_set()"));
    IRBuilder ir(pgm.cc);

    // get container pointer
    Operand &obj_op = pgm.tkFunction->voperand(pgm, &object);
    x86::Gp obj_reg = pgm.cc.newIntPtr("sub_obj");
    pgm.cc.mov(obj_reg, obj_op.as<x86::Gp>());

    // compile index expression
    regdefp_t idx_rdp = {nullptr, nullptr, nullptr};
    Operand &idx_op = index->compile(pgm, idx_rdp);
    x86::Gp idx_reg = pgm.cc.newGpq("sub_idx");
    if ( idx_op.isReg() )
        pgm.cc.mov(idx_reg, idx_op.as<x86::Gp>());
    else
        pgm.cc.mov(idx_reg, idx_op.as<Imm>());

    DataType ctype = object.type->type();

    // C fixed-size array: store element directly to [base + linear_idx*elem_size]
    if ( object.is_fixed_array() )
    {
        size_t elem_size = object.type->size ? object.type->size : 8;
        uint32_t shift = 0;
        if      ( elem_size == 8 ) shift = 3;
        else if ( elem_size == 4 ) shift = 2;
        else if ( elem_size == 2 ) shift = 1;
        DBG(pgm.cc.comment("fixed-array subscript write"));
        // Fold extra indices into idx_reg using dims: linear = ((i0*d1)+i1)*d2 + i2 ...
        for ( size_t k = 0; k < extra_indices.size(); ++k )
        {
            uint32_t dim_k = object.dims[k + 1];
            pgm.cc.imul(idx_reg, idx_reg, imm((int64_t)dim_k));
            regdefp_t ex_rdp = {nullptr, nullptr, nullptr};
            Operand &ex_op = extra_indices[k]->compile(pgm, ex_rdp);
            if ( ex_op.isReg() )
                pgm.cc.add(idx_reg, ex_op.as<x86::Gp>());
            else
                pgm.cc.add(idx_reg, ex_op.as<Imm>());
        }
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
        ir.store(IRValue::mem(elem_mem, _datatype), ir_from_operand(val_op, val_type ? val_type : _datatype));
        return;
    }

    // Raw pointer subscript write: ptr[i] = val  →  *(ptr + i*elem) = val
    if ( !object.is_fixed_array() && object.type->is_pointer() )
    {
        DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(object.type);
        DataDef *base = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
        size_t elem_size = base->size ? base->size : 8;
        uint32_t shift = 0;
        if      ( elem_size == 8 ) shift = 3;
        else if ( elem_size == 4 ) shift = 2;
        else if ( elem_size == 2 ) shift = 1;
        DBG(pgm.cc.comment("pointer subscript write"));
        x86::Mem elem_mem = x86::ptr(obj_reg, idx_reg, shift, 0, (uint32_t)elem_size);
        ir.store(IRValue::mem(elem_mem, base), ir_from_operand(val_op, val_type ? val_type : base));
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
    std::map<Variable *, Operand>::iterator rmi;

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
	if ( var->is_global() && var->data && !var->is_fixed_array() )
	{
	    DBG(pgm.cc.comment("TokenCpnd::voperand() variable found, var->is_global() && var->data"));
	    movreg(pgm.cc, rmi->second, var);
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
	    x86::Gp env_gp = env_op.as<x86::Gp>();
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
    // C fixed-size array: allocate stack slot (local) or load heap pointer
    // (global / static-local). The operand is the base pointer; subscript
    // code adds index*elem_size.
    if ( var->is_fixed_array() )
    {
	x86::Gp reg = pgm.cc.newIntPtr("%s", var->name.c_str());
	if ( var->data )
	{
	    // Global or static-local: parseDeclaration calloc'd the backing
	    // storage. Load its absolute address.
	    DBG(pgm.cc.comment("voperand fixed-size array (global/static)"));
	    pgm.cc.mov(reg, imm(var->data));
	}
	else
	{
	    size_t elem_size = var->type->size ? var->type->size : 8;
	    size_t total = elem_size * var->total_elements();
	    uint32_t align = (uint32_t)(elem_size < 8 ? elem_size : 8);
	    DBG(pgm.cc.comment("voperand fixed-size array (stack)"));
	    x86::Mem stack = pgm.cc.newStack((uint32_t)total, align);
	    pgm.cc.lea(reg, stack);
	}
	operand_map[var] = reg;
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
	uint32_t align = (uint32_t)(var->type->size < 8 ? var->type->size : 8);
	if ( align == 0 )
	    align = 8;
	x86::Mem stack = pgm.cc.newStack((uint32_t)var->type->size, align);
	stack.setSize((uint32_t)var->type->size);
	operand_map[var] = stack;
	var->flags |= vfSTACKSET;

	if ( !(var->flags & vfPARAM) )
	{
	    Operand tmp = var->type->newreg(pgm.cc, var->name.c_str());
	    if ( var->type->is_integer() && tmp.isReg() && tmp.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.safexor(tmp, tmp);
	    else
	    if ( var->type->is_real() && tmp.isReg() && tmp.as<BaseReg>().isGroup(RegGroup::kVec) )
		pgm.cc.xorps(tmp.as<x86::Xmm>(), tmp.as<x86::Xmm>());
	    IRBuilder ir(pgm.cc);
	    ir.store(IRValue::mem(stack, var->type), ir_from_operand(tmp, var->type));
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
		    // align stack to struct's max member alignment (C ABI compatible)
		    size_t struct_align = 8;
		    if ( var->type->basetype() == BaseType::btStruct )
			struct_align = static_cast<DataDefSTRUCT *>(var->type)->max_align;
		    x86::Mem stack = pgm.cc.newStack(var->type->size, (uint32_t)struct_align);
		    operand_map[var] = stack;

		    // Construct any non-trivial members (strings, streams) inside the struct
		    DataDefSTRUCT *dds = static_cast<DataDefSTRUCT *>(var->type);
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
	    movreg(pgm.cc, rmi->second, var); // first initialization of non-stack register (regset)
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
void TokenCpnd::putreg(asmjit::x86::Compiler &cc, Variable *var)
{
    // shortcut out if we can't work with this variable
    if ( !(var->is_global() && var->data && (var->flags & vfREGSET) && (var->flags & vfMODIFIED) && var->type->is_numeric()) )
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
    DBG(cc.comment("TokenCpnd::putreg() calling cc.mov(var->data, reg)"));
    if ( rmi->second.isReg() && rmi->second.as<BaseReg>().isGroup(RegGroup::kGp) )
	var->type->movrval2mptr(cc, var->data, rmi->second.as<x86::Gp>());

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
	if ( (rmi->first->flags & vfSTACK) )
	{
	    // Don't destruct parameter objects — the caller owns them
	    if ( (rmi->first->flags & vfPARAM) )
		continue;
	    if ( rmi->first->type->type() > DataType::dtRESERVED )
	    {
		Operand &reg = rmi->second;
		Variable *var = rmi->first;

		switch(var->type->type())
		{
		    case DataType::dtSTRING:
			{
			    DBG(std::cout << "TokenCpnd::cleanup(" << var->name << ") calling string_destruct[" << (uint64_t)string_destruct << ']' << std::endl);
                            InvokeNode* call; cc.invoke(&call, imm(string_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtSSTREAM:
			{
                            InvokeNode* call; cc.invoke(&call, imm(stringstream_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtARRAY:
			{
                            InvokeNode* call; cc.invoke(&call, imm(madarray_destruct), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtVECTOR:
			{
			    DataDefVECTOR *vdd = static_cast<DataDefVECTOR *>(var->type);
			    void *dtor = vdd->element_type->is_string()
				? (void *)vector_str_destruct : (void *)vector_int_destruct;
			    InvokeNode* call; cc.invoke(&call, imm(dtor), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtMAP:
			{
			    DataDefMAP *mdd = static_cast<DataDefMAP *>(var->type);
			    void *dtor = mdd->val_type->is_string()
				? (void *)map_str_str_destruct : (void *)map_str_int_destruct;
			    InvokeNode* call; cc.invoke(&call, imm(dtor), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
			}
			break;
		    case DataType::dtSET:
			{
			    DataDefSET *sdd = static_cast<DataDefSET *>(var->type);
			    void *dtor = sdd->element_type->is_string()
				? (void *)set_str_destruct : (void *)set_int_destruct;
			    InvokeNode* call; cc.invoke(&call, imm(dtor), FuncSignature::build<void, void *>());
			    call->setArg(0, reg.as<x86::Gp>());
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
    if ( (left && left->is_real()) || (right && right->is_real()) )
	regdp.second = &ddDOUBLE;
    else
    if ( (left && left->datadef()->is_integer() ) )
	regdp.second = left->datadef();
    else
    if ( (right && right->datadef()->is_integer() ) )
	regdp.second = right->datadef();
    else
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
	DBG(pgm.cc.comment("optimize() regdp.second = &ddINT"));
	regdp.second = &ddINT;
    }
    if ( !regdp.first )
    {
	_operand = pgm.cc.newGpq("_operand_Gpq_");
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
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	return emit_plain_binop3(pgm, left, right, regdp.second, &Program::safeadd, regdp, _operand, "_add_l");
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
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	return emit_plain_binop3(pgm, left, right, regdp.second, &Program::safesub, regdp, _operand, "_sub_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenSub::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    emit_pointer_arith_scale(pgm, left, right, rval);	 // ptr - n → ptr - n*sizeof(*ptr)
    pgm.safesub(lval, rval, regdp.second);
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
	pgm.safeneg(rval);
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
    pgm.safeneg(rval);					 // type safe negation
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
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	return emit_plain_binop3(pgm, left, right, regdp.second, &Program::safemul, regdp, _operand, "_mul_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenMul::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safemul(lval, rval, regdp.second);
    return finish_general_binop(pgm, regdp, lval, c);
}

// divide two numbers
Operand &TokenDiv::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenDiv::Compile() TOP" << endl);
    if ( !left )  { throw "/ missing lval operand"; } 
    if ( !right ) { throw "/ missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right) && regdp.second && !regdp.second->is_pointer() )
	return emit_plain_divmod(pgm, left, right, regdp.second, /*return_remainder=*/false, regdp, _operand, "_div_l");
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

    settype(pgm, regdp);				 // set regdp.second type
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_bitop2(pgm, left, right, regdp.second, &Program::safeshl,
				 /*right_must_be_gp=*/true, regdp, _operand, "_shl_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBSL::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeshl(lval, rval);
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
    settype(pgm, regdp);				 // set regdp.second type
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_bitop2(pgm, left, right, regdp.second, &Program::safeshr,
				 /*right_must_be_gp=*/true, regdp, _operand, "_shr_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBSR::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeshr(lval, rval);
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
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_bitop2(pgm, left, right, regdp.second, &Program::safeor,
				 /*right_must_be_gp=*/true, regdp, _operand, "_or_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBor::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeor(lval, rval);
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
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_bitop2(pgm, left, right, regdp.second, &Program::safexor,
				 /*right_must_be_gp=*/true, regdp, _operand, "_xor_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenXor::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safexor(lval, rval);
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
    if ( is_plain_numeric_expr(left) && is_plain_numeric_expr(right)
      && regdp.second && regdp.second->is_integer() )
	return emit_plain_bitop2(pgm, left, right, regdp.second, &Program::safeand,
				 /*right_must_be_gp=*/true, regdp, _operand, "_and_l");
    GeneralBinopCascade c = begin_general_binop(pgm, regdp, _operand);
    Operand &lval = left->compile(pgm, regdp);
    if ( !regdp.second ) { throw "TokenBand::compile() left->compile() cleared datatype!"; }
    Operand tmp = regdp.second->newreg(pgm.cc, "tmp");
    regdp.first = &tmp;
    Operand &rval = right->compile(pgm, regdp);
    pgm.safeand(lval, rval);
    return finish_general_binop(pgm, regdp, lval, c);
}

// bitwise not ~
Operand &TokenBnot::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(cout << "TokenBnot::Compile() TOP" << endl);
    if ( left )   { throw "Bitwise not has lval!"; }
    if ( !right ) { throw "~ missing rval operand"; }
    if ( can_optimize() ) {return optimize(pgm, regdp);} // attempt optimization
    settype(pgm, regdp);				 // set regdp.second type
    if ( right && right->datadef() && right->datadef()->is_integer() )
    {
	Operand *caller_dest = regdp.first;
	Operand out_norm;
	Operand &rval = compile_token_gp_normalized(pgm, right, regdp.second, out_norm);
	pgm.safenot(rval);
	_operand = rval;
	regdp.first = &_operand;
	if ( caller_dest )
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
    Operand &lval = compile_token_normalized(pgm, left, test_type, nullptr, left_storage);
    Operand &rval = compile_token_normalized(pgm, right, test_type, nullptr, right_storage);
    x86::Gp result = pgm.cc.newGpq("_lor");
    pgm.testzero(lval);					 // test lval is 0
    pgm.safesetne(result);				 // if lval !=0, ret(result) = 1
    pgm.cc.jne(done);					 // if lval != 0, jump to done
    pgm.testzero(rval);					 // test rval is 0
    pgm.safesetne(result);				 // if rval !=0, ret(result) = 1
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
    Operand &lval = compile_token_normalized(pgm, left, test_type, nullptr, left_storage);
    Operand &rval = compile_token_normalized(pgm, right, test_type, nullptr, right_storage);
    x86::Gp result = pgm.cc.newGpq("_land");
    pgm.testzero(lval);					 // test lval is 0
    pgm.safesetne(result);				 // if lval !=0, ret(result) = 1
    pgm.cc.je(done);					 // if lval == 0, jump to done
    pgm.testzero(rval);					 // test rval is 0
    pgm.safesetne(result);				 // if rval !=0, ret(result) = 1
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
    pgm.safecmp(lval, rval);				 // typesafe comparison

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
	else if ( method->x86code )
	    pgm.cc.mov(addr_gp, imm(method->x86code));

	if ( !regdp.second )
	    regdp.second = var.type;
	return emit_ir_value(pgm, IRValue::reg(addr_gp, var.type), regdp, _operand, var.type);
    }

    DBG(pgm.cc.comment("TokenVar::compile() reg = operand()"));
    Operand &reg = operand(pgm);

    if ( !regdp.second )
	regdp.second = _datatype;

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

    if ( (_datatype->is_numeric() || _datatype->is_pointer()) && (reg.isMem() || reg.isReg() || reg.isImm()) )
	return emit_ir_value(pgm, ir_from_operand(reg, _datatype), regdp, _operand, _datatype);

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
    // return Mem operand [ptr] for numeric types (enables read/write)
    if ( deref_type->is_numeric() )
	_operand = x86::ptr(ptr_gp, 0, (uint32_t)deref_type->size);
    else
	_operand = ptr_gp; // non-numeric: pointer value IS the address
    return _operand;
}

Operand &TokenDeref::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenDeref::compile()"));
    if ( !regdp.second )
	regdp.second = deref_type;

    Operand &mem = operand(pgm);

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
    x86::Gp ptr_gp = ptr_op.as<x86::Gp>();
    if ( deref_type->is_numeric() )
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

    if ( deref_type->is_numeric() )
    {
	x86::Mem mem = x86::ptr(old_ptr, 0, (uint32_t)deref_type->size);
	if ( regdp.first )
	{
	    pgm.safemov(*regdp.first, mem, deref_type, deref_type);
	    return *regdp.first;
	}
	x86::Gp gp = pgm.cc.newGpq(increment ? "*postinc" : "*postdec");
	pgm.safemov(gp, mem, deref_type, deref_type);
	_operand = gp;
	regdp.first = &_operand;
	regdp.second = deref_type;
	return _operand;
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
    if ( cast_type && cast_type->rawtype() == DataType::dtVOID )
    {
	regdefp_t inner_rdp = {NULL, expr ? expr->datadef() : NULL, NULL};
	Operand &result = expr->compile(pgm, inner_rdp);
	regdp.second = cast_type;
	if ( !regdp.first )
	    regdp.first = &result;
	return result;
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
	&& cast_type->rawtype() == DataType::dtCHAR;
    if ( str_to_cstr )
    {
	regdefp_t inner_rdp = {NULL, NULL, NULL};
	Operand &str_op = expr->compile(pgm, inner_rdp);
	x86::Gp cstr = pgm.cc.newIntPtr("cast_cstr");
	InvokeNode *call;
	pgm.cc.invoke(&call, imm(string_cstr), FuncSignature::build<const char *, void *>());
	call->setArg(0, str_op.as<x86::Gp>());
	call->setRet(0, cstr);
	regdp.second = cast_type;
	if ( regdp.first && regdp.first->isReg() && regdp.first->as<BaseReg>().isGroup(RegGroup::kGp) )
	{
	    pgm.cc.mov(regdp.first->as<x86::Gp>(), cstr);
	    return *regdp.first;
	}
	if ( regdp.first && regdp.first->isMem() )
	{
	    pgm.cc.mov(regdp.first->as<x86::Mem>(), cstr);
	    return *regdp.first;
	}
	_operand = cstr;
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

    // set the result type to the cast target
    regdp.second = cast_type;
    // compile the inner expression — for pointer/integer casts, the value
    // is the same (all 64-bit), so just compile and reinterpret the type
    Operand &result = expr->compile(pgm, regdp);
    regdp.second = cast_type; // ensure type is set after compile
    return result;
}

// & address-of operator: emit LEA to get address of variable
Operand &TokenAddrOf::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenAddrOf::compile()"));
    Operand &obj = pgm.tkFunction->voperand(pgm, &var);

    x86::Gp addr = pgm.cc.newIntPtr("%s", ("&" + var.name).c_str());
    if ( var.is_global() && var.data && !var.is_fixed_array() )
	pgm.cc.mov(addr, imm(var.data));
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
    if ( dynamic_cast<TokenMember *>(expr) )
    {
	Operand &obj = expr->operand(pgm);
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
	    pgm.cc.mov(addr, imm(v.data));
	else if ( obj.isMem() )
	    pgm.cc.lea(addr, obj.as<x86::Mem>());
	else
	    pgm.cc.mov(addr, obj.as<x86::Gp>());
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

// va_arg(ap, type) — read next variadic argument from packed buffer and advance
Operand &TokenVaArg::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenVaArg::compile()"));

    // get the va_list variable's operand (it's an int64_t holding a pointer)
    Operand &ap_op = pgm.tkFunction->voperand(pgm, ap_var);

    // load current pointer value into a temp register
    x86::Gp ptr = pgm.cc.newIntPtr("va_ptr");
    if ( ap_op.isReg() )
	pgm.cc.mov(ptr, ap_op.as<x86::Gp>());
    else
	pgm.cc.mov(ptr, ap_op.as<x86::Mem>());

    // read value at [ptr] into result register
    x86::Gp result = pgm.cc.newGpq("va_val");
    pgm.cc.mov(result, x86::qword_ptr(ptr));

    // advance ap by 8 bytes
    pgm.cc.add(ptr, 8);
    if ( ap_op.isReg() )
	pgm.cc.mov(ap_op.as<x86::Gp>(), ptr);
    else
	pgm.cc.mov(ap_op.as<x86::Mem>(), ptr);

    if ( !regdp.second )
	regdp.second = target_type;
    return emit_ir_value(pgm, IRValue::reg(result, &ddINT64), regdp, _operand, target_type);
}

// load double into operand
Operand &TokenReal::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenReal::compile()"));
    if ( !regdp.second )
    {
	if ( !_datatype ) { throw "TokenReal has NULL _datatype"; }
	regdp.second = _datatype;
	DBG(pgm.cc.comment("TokenReal::compile() setting _datatype to double"));
    }
    _const = pgm.cc.newDoubleConst(ConstPoolScope::kLocal, _val);
    if ( _datatype->size == sizeof(float) )
	_const.setSize(sizeof(float));
    else
	_const.setSize(sizeof(double));
    DBG(pgm.cc.comment("TokenReal::compile() emitting through IRBuilder"));
    DBG(cout << "TokenReal::compile() emitting through IRBuilder const[" << _val << "]" << endl);
    return emit_ir_value(pgm, IRValue::mem(_const, _datatype), regdp, _operand, _datatype);
}

// load integer into register
Operand &TokenInt::compile(Program &pgm, regdefp_t &regdp)
{
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

    if ( regdp.second && (regdp.second->is_numeric() || regdp.second->is_pointer()) )
    {
	uint32_t merge_size = (uint32_t)(regdp.second->size ? regdp.second->size : 8);
	uint32_t merge_align = merge_size < 8 ? merge_size : 8;
	x86::Mem merge_slot = pgm.cc.newStack(merge_size, merge_align);
	merge_slot.setSize(merge_size);
	Operand merge_op = merge_slot;

	{
	    regdefp_t trdp = {&merge_op, regdp.second, NULL};
	    true_expr->compile(pgm, trdp);
	}
	pgm.cc.jmp(L_end);

	pgm.cc.bind(L_false);
	{
	    regdefp_t frdp = {&merge_op, regdp.second, NULL};
	    false_expr->compile(pgm, frdp);
	}
	pgm.cc.bind(L_end);

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
	x86::Gp rb_gp = rb_op.as<x86::Gp>();
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
	if ( ret_type && ret_type->is_pointer()
	  && ret_type->rawtype() == DataType::dtCHAR
	  && returns->datadef() && returns->datadef()->rawtype() == DataType::dtSTRING )
	{
	    regdefp_t rhs_rdp = {NULL, NULL, NULL};
	    Operand &str_obj = returns->compile(pgm, rhs_rdp);
	    x86::Gp str_gp = str_obj.as<x86::Gp>();
	    InvokeNode *call;
	    pgm.cc.invoke(&call, imm(string_cstr), FuncSignature::build<const char *, void *>());
	    call->setArg(0, str_gp);
	    x86::Gp cstr_gp = pgm.cc.newIntPtr("__ret_cstr");
	    call->setRet(0, cstr_gp);
	    _operand = cstr_gp;
	    pgm.saferet(cstr_gp);
	    return _operand;
	}

	Operand ret_storage;
	Operand &reg = (ret_type && (ret_type->is_numeric() || ret_type->is_pointer()))
	    ? compile_token_normalized(pgm, returns, ret_type, NULL, ret_storage)
	    : returns->compile(pgm, regdp);
	pgm.saferet(reg);
	return reg;
    }
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
	if ( normalized_switch )
	{
	    Operand &val_op = compile_token_gp_normalized(pgm, cases[i]->value, expr_type, val_storage);
	    pgm.cc.cmp(expr_reg, val_op.as<x86::Gp>());
	}
	else
	{
	    regdefp_t valrdp = {NULL, NULL, NULL};
	    Operand &val_op = cases[i]->value->compile(pgm, valrdp);
	    if ( val_op.isReg() && val_op.as<BaseReg>().isGroup(RegGroup::kGp) )
		pgm.cc.cmp(expr_reg, val_op.as<x86::Gp>());
	    else if ( val_op.isImm() )
		pgm.cc.cmp(expr_reg, val_op.as<Imm>());
	    else if ( val_op.isMem() )
		pgm.cc.cmp(expr_reg, val_op.as<x86::Mem>());
	}
	pgm.cc.je(case_labels[i]);
    }
    // fall through to default or exit
    pgm.cc.jmp(defaultcase ? default_label : sw_exit);

    // push exit label onto loopstack so break works
    pgm.loopstack.push(make_pair((Label *)NULL, &sw_exit));

    // emit case bodies (fall-through between cases)
    for ( size_t i = 0; i < cases.size(); ++i )
    {
	pgm.cc.bind(case_labels[i]);
	DBG(pgm.cc.comment("case body"));
	for ( auto *stmt : cases[i]->statements )
	{
	    regdefp_t stmtrdp = {NULL, NULL, NULL};
	    stmt->compile(pgm, stmtrdp);
	}
    }

    // emit default body
    if ( defaultcase )
    {
	pgm.cc.bind(default_label);
	DBG(pgm.cc.comment("default body"));
	for ( auto *stmt : defaultcase->statements )
	{
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
