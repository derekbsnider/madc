//////////////////////////////////////////////////////////////////////////
//									//
// Control-flow compile methods: if, for, while, do, foreach,		//
// switch/case, goto, label, break, continue, return, match.		//
//									//
// Extracted from compiler.cpp — Phase A step 3 (file splitting).	//
//									//
//////////////////////////////////////////////////////////////////////////
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <queue>
#include <stack>
#include <map>
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

// extern declarations for php array helpers (defined in ns_php.cpp)
extern int64_t php_count(void *arr);
extern void *php_array_get(void *result, void *arr, int64_t index);
extern int64_t php_array_get_int(void *arr, int64_t index);

// extern declarations for STL container helpers (defined in ns_stl.cpp)
extern int64_t vector_int_size(void *);
extern int64_t vector_int_at(void *, int64_t);
extern void *vector_str_at(void *, void *, int64_t);
extern int64_t vector_str_size(void *);


///////////////////////////////////////////////////////////////////////////
//  Helpers							//
///////////////////////////////////////////////////////////////////////////

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

Operand &compile_condition_operand(Program &pgm, TokenBase *condition, Operand &storage)
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

// Check if a statement subtree contains any labels (TokenLabel or
// TokenCASE) that might be jump targets.  Used to decide whether an
// if(0) then-block can safely be elided.
static bool contains_label(TokenBase *node)
{
    return walk_ast(node, [](TokenBase *n) {
	return dynamic_cast<TokenLabel *>(n) != nullptr
	    || dynamic_cast<TokenCASE *>(n) != nullptr;
    });
}

// Function-scope label-map accessor. Creates an asmjit Label on
// first reference so forward-referenced gotos resolve without
// ordering requirements. `TokenFunc::compile` clears
// `pgm.label_map` at each function boundary.
Label &lookup_or_make_label(Program &pgm, const std::string &name)
{
    auto it = pgm.label_map.find(name);
    if ( it != pgm.label_map.end() )
	return it->second;
    pgm.label_map[name] = pgm.cc.newLabel();
    return pgm.label_map[name];
}

///////////////////////////////////////////////////////////////////////////
//  Return / Break / Continue					//
///////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////
//  Goto / Label							//
///////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////
//  Switch / Case / Match					//
///////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////
//  If / While / Do / For / Foreach				//
///////////////////////////////////////////////////////////////////////////

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

// --- Exception handling: try/catch/throw ---

// Runtime function declarations (defined in exception_runtime.cpp)
extern "C" {
    void *__madc_try_push(void *ctx);
    void __madc_try_pop();
    void __madc_throw_int(int64_t val);
    void __madc_throw_double(double val);
    void __madc_throw_cstr(const char *val);
    int __madc_exception_type();
    int64_t __madc_exception_int();
    double __madc_exception_double();
    const char *__madc_exception_cstr();
    void __madc_exception_clear();
}

// sizeof(MadcTryContext) = sizeof(jmp_buf) + sizeof(void*)
// jmp_buf on Linux x86-64 is typically 200 bytes; add 8 for prev pointer
static const uint32_t TRYCTX_SIZE = 208;
static const uint32_t TRYCTX_ALIGN = 16;

Operand &TokenTHROW::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenTHROW::compile()"));

    if ( !throw_expr )
    {
	// rethrow — not yet implemented
	pgm.Throw(this) << "rethrow (throw;) not yet implemented" << flush;
    }

    // Compile the throw expression
    regdefp_t erdp = {NULL, NULL, NULL};
    Operand &val = throw_expr->compile(pgm, erdp);
    DataDef *vtype = erdp.second ? erdp.second : throw_expr->datadef();

    // Determine which throw function to call based on type
    void *throw_fn = (void *)__madc_throw_int; // default
    if ( vtype && (vtype->is_real() || vtype->type() == DataType::dtFLOAT) )
	throw_fn = (void *)__madc_throw_double;
    else if ( vtype && (vtype->type() == DataType::dtCHARptr
	  || vtype->type() == DataType::dtSTRING) )
	throw_fn = (void *)__madc_throw_cstr;

    InvokeNode *call;
    if ( throw_fn == (void *)__madc_throw_double )
    {
	// Double argument
	x86::Xmm xarg = pgm.cc.newXmm("__throw_dbl");
	if ( val.isReg() && val.as<BaseReg>().isGroup(RegGroup::kVec) )
	    pgm.cc.movsd(xarg, val.as<x86::Xmm>());
	else if ( val.isMem() )
	    pgm.cc.movsd(xarg, val.as<x86::Mem>());
	pgm.cc.invoke(&call, imm(throw_fn),
		      FuncSignature::build<void, double>());
	call->setArg(0, xarg);
    }
    else
    {
	// Int or string argument — widen to 64-bit
	x86::Gp garg = pgm.cc.newGpq("__throw_val");
	if ( val.isReg() )
	{
	    x86::Gp src = val.as<x86::Gp>();
	    if ( src.size() < 8 )
		pgm.cc.movsxd(garg, src);
	    else
		pgm.cc.mov(garg, src);
	}
	else if ( val.isMem() )
	    pgm.cc.mov(garg, val.as<x86::Mem>());
	else if ( val.isImm() )
	    pgm.cc.mov(garg, val.as<Imm>());
	pgm.cc.invoke(&call, imm(throw_fn),
		      FuncSignature::build<void, int64_t>());
	call->setArg(0, garg);
    }

    return _operand;
}

Operand &TokenTRY::compile(Program &pgm, regdefp_t &regdp)
{
    DBG(pgm.cc.comment("TokenTRY::compile()"));

    // Allocate MadcTryContext on JIT stack
    x86::Mem tryctx = pgm.cc.newStack(TRYCTX_SIZE, TRYCTX_ALIGN);

    // Push try context: jbuf_ptr = __madc_try_push(&tryctx)
    x86::Gp ctx_ptr = pgm.cc.newIntPtr("__tryctx");
    pgm.cc.lea(ctx_ptr, tryctx);
    InvokeNode *push_call;
    pgm.cc.invoke(&push_call, imm((void *)__madc_try_push),
		  FuncSignature::build<void *, void *>());
    push_call->setArg(0, ctx_ptr);
    x86::Gp jbuf_ptr = pgm.cc.newIntPtr("__jbuf");
    push_call->setRet(0, jbuf_ptr);

    // Call setjmp(jbuf_ptr) — returns 0 on setup, non-zero on exception
    void *setjmp_fn = dlsym(RTLD_DEFAULT, "_setjmp");
    if ( !setjmp_fn )
	setjmp_fn = dlsym(RTLD_DEFAULT, "setjmp");
    if ( !setjmp_fn )
	pgm.Throw(this) << "cannot resolve setjmp" << flush;

    FuncSignature setjmp_sig(CallConvId::kCDecl);
    setjmp_sig.setRetT<int>();
    setjmp_sig.addArgT<void *>();

    InvokeNode *sjcall;
    pgm.cc.invoke(&sjcall, imm(setjmp_fn), setjmp_sig);
    sjcall->setArg(0, jbuf_ptr);
    x86::Gp sjret = pgm.cc.newGpq("__sjret");
    sjcall->setRet(0, sjret);

    // Branch: if setjmp returned 0, execute try body; else go to catch dispatch
    Label catch_dispatch = pgm.cc.newLabel();
    Label after_catch = pgm.cc.newLabel();
    pgm.cc.cmp(sjret, imm(0));
    pgm.cc.jne(catch_dispatch);

    // --- Try body ---
    {
	regdefp_t body_rdp = {NULL, NULL, NULL};
	try_body->compile(pgm, body_rdp);
    }

    // Normal exit: pop the try context
    InvokeNode *pop_call;
    pgm.cc.invoke(&pop_call, imm((void *)__madc_try_pop),
		  FuncSignature::build<void>());
    pgm.cc.jmp(after_catch);

    // --- Catch dispatch ---
    pgm.cc.bind(catch_dispatch);

    // Get exception type
    InvokeNode *type_call;
    pgm.cc.invoke(&type_call, imm((void *)__madc_exception_type),
		  FuncSignature::build<int>());
    x86::Gp exc_type = pgm.cc.newGpd("__exc_type");
    type_call->setRet(0, exc_type);

    for ( size_t i = 0; i < catch_types.size(); ++i )
    {
	Label next_catch = pgm.cc.newLabel();
	Label this_catch = pgm.cc.newLabel();

	if ( catch_types[i] != 99 ) // not catch(...)
	{
	    pgm.cc.cmp(exc_type, imm(catch_types[i]));
	    pgm.cc.jne(next_catch);
	}

	pgm.cc.bind(this_catch);

	// Bind exception value to catch variable if named.
	// The variable lives in the catch body's compound (created at parse time).
	// Use the compound's voperand to get its storage.
	TokenCpnd *catch_cpnd = dynamic_cast<TokenCpnd *>(catch_bodies[i]);
	if ( !catch_varnames[i].empty() && catch_cpnd )
	{
	    Variable *cv = catch_cpnd->findVariable(catch_varnames[i]);
	    if ( cv )
	    {
		if ( catch_types[i] == 1 ) // int
		{
		    InvokeNode *val_call;
		    pgm.cc.invoke(&val_call, imm((void *)__madc_exception_int),
				  FuncSignature::build<int64_t>());
		    x86::Gp exc_val = pgm.cc.newGpq("__exc_int");
		    val_call->setRet(0, exc_val);
		    Operand &cv_op = pgm.tkFunction->voperand(pgm, cv);
		    if ( cv_op.isMem() )
			pgm.cc.mov(cv_op.as<x86::Mem>(), exc_val);
		    else
			pgm.cc.mov(cv_op.as<x86::Gp>(), exc_val);
		}
		else if ( catch_types[i] == 2 ) // double
		{
		    InvokeNode *val_call;
		    pgm.cc.invoke(&val_call, imm((void *)__madc_exception_double),
				  FuncSignature::build<double>());
		    x86::Xmm exc_val = pgm.cc.newXmm("__exc_dbl");
		    val_call->setRet(0, exc_val);
		    Operand &cv_op = pgm.tkFunction->voperand(pgm, cv);
		    if ( cv_op.isMem() )
			pgm.cc.movsd(cv_op.as<x86::Mem>(), exc_val);
		    else
			pgm.cc.movsd(cv_op.as<x86::Xmm>(), exc_val);
		}
	    }
	}

	// Compile catch body
	{
	    regdefp_t catch_rdp = {NULL, NULL, NULL};
	    catch_bodies[i]->compile(pgm, catch_rdp);
	}

	// Clear exception
	InvokeNode *clear_call;
	pgm.cc.invoke(&clear_call, imm((void *)__madc_exception_clear),
		      FuncSignature::build<void>());

	pgm.cc.jmp(after_catch);
	pgm.cc.bind(next_catch);
    }

    // No catch matched — should not happen in well-formed code
    // (unmatched exceptions abort in __madc_throw_*)

    pgm.cc.bind(after_catch);

    return _operand;
}

