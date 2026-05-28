/* madc_cir.cpp — CIR translation: TokenBase AST → c2mir node_t tree.
 *
 * Walks the madc parser's TokenBase tree and builds the equivalent
 * c2mir node_t tree, which can then be fed to c2mir's type checker
 * and MIR generator.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdint.h>

#include <asmjit/x86.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_cir.h"

extern "C" {
#include "c2mir/c2mir_api.h"
}

extern thread_local bool madc_verbose;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static c2mir_pos_t make_pos(TokenBase *tb)
{
    c2mir_pos_t p;
    p.fname = tb ? tb->file : "unknown";
    p.lno = tb ? tb->line : 0;
    p.ln_pos = tb ? tb->column : 0;
    return p;
}

static c2mir_pos_t no_pos()
{
    c2mir_pos_t p = { NULL, -1, -1 };
    return p;
}

// -----------------------------------------------------------------------
// Type translation: DataDef → c2mir type specifier node
// -----------------------------------------------------------------------

static node_t cir_type_spec(c2m_ctx_t c2m, DataDef *dd)
{
    if (!dd) return c2mir_new_node(c2m, N_INT);  // default to int

    DataType dt = dd->rawtype();
    switch (dt) {
    case DataType::dtVOID:   return c2mir_new_node(c2m, N_VOID);
    case DataType::dtCHAR:   return c2mir_new_node(c2m, N_CHAR);
    //case DataType::dtINT8: return c2mir_new_node(c2m, N_CHAR);  // same as dtCHAR
    case DataType::dtINT16:  return c2mir_new_node(c2m, N_SHORT);
    case DataType::dtINT32:  return c2mir_new_node(c2m, N_INT);
    case DataType::dtINT64: {
	// long long on most platforms
	node_t list = c2mir_new_node(c2m, N_LIST);
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_LONG));
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_LONG));
	return list;  // caller must flatten
    }
    case DataType::dtUINT8:
    case DataType::dtUINT16:
    case DataType::dtUINT32:
    case DataType::dtUINT64: {
	node_t u = c2mir_new_node(c2m, N_UNSIGNED);
	// For simplicity, return just 'unsigned' — caller adds size
	return u;
    }
    case DataType::dtFLOAT:  return c2mir_new_node(c2m, N_FLOAT);
    case DataType::dtDOUBLE: return c2mir_new_node(c2m, N_DOUBLE);
    default:
	return c2mir_new_node(c2m, N_INT);
    }
}

// Build a type specifier LIST node from a DataDef.
// Returns N_LIST(type_spec, ...) matching c2mir's declaration_specifiers.
static node_t cir_type_list(c2m_ctx_t c2m, DataDef *dd, c2mir_pos_t pos)
{
    node_t list = c2mir_new_node(c2m, N_LIST);
    node_t spec = cir_type_spec(c2m, dd);
    // If cir_type_spec returned a LIST (for long long), flatten it
    // For now, simple types return a single node
    c2mir_op_append(c2m, list, spec);
    c2mir_set_node_pos(c2m, list, pos);
    return list;
}

// -----------------------------------------------------------------------
// Expression translation: TokenBase → c2mir expr node
// -----------------------------------------------------------------------

static node_t cir_translate_expr(c2m_ctx_t c2m, TokenBase *tb)
{
    if (!tb) return c2mir_new_node(c2m, N_IGNORE);

    c2mir_pos_t pos = make_pos(tb);

    // Integer literal
    if (tb->type() == TokenType::ttInteger) {
	return c2mir_new_i_node(c2m, tb->ival(), pos);
    }

    // Real literal
    if (tb->type() == TokenType::ttReal) {
	return c2mir_new_d_node(c2m, tb->dval(), pos);
    }

    // String literal
    if (tb->type() == TokenType::ttString) {
	TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
	if (ti) {
	    return c2mir_new_str_node(c2m, N_STR, ti->str.c_str(),
				     ti->str.size() + 1, pos);
	}
    }

    // Variable reference
    if (tb->type() == TokenType::ttVariable) {
	TokenVar *tv = dynamic_cast<TokenVar *>(tb);
	if (tv) {
	    return c2mir_new_str_node(c2m, N_ID, tv->var.name.c_str(),
				     tv->var.name.size() + 1, pos);
	}
    }

    // Identifier (not yet resolved to a variable)
    if (tb->type() == TokenType::ttIdentifier) {
	TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
	if (ti) {
	    return c2mir_new_str_node(c2m, N_ID, ti->str.c_str(),
				     ti->str.size() + 1, pos);
	}
    }

    // Operators (unary and binary)
    if (tb->is_operator()) {
	TokenOperator *top = dynamic_cast<TokenOperator *>(tb);

	// Unary operators (argc == 1, operand in right)
	if (top && top->argc() == 1 && top->right) {
	    node_t operand = cir_translate_expr(c2m, top->right);

	    if (tb->id() == TokenID::tkNeg) {
		// Unary minus: 0 - operand
		node_t zero = c2mir_new_i_node(c2m, 0, pos);
		node_t n = c2mir_new_node2(c2m, N_SUB, zero, operand);
		c2mir_set_node_pos(c2m, n, pos);
		return n;
	    }

	    c2mir_node_code_t code;
	    switch (tb->id()) {
	    case TokenID::tkLnot:  code = N_NOT; break;
	    case TokenID::tkBnot:  code = N_BITWISE_NOT; break;
	    default:
		DBG(std::cerr << "cir: unhandled unary op " << (int)tb->id() << std::endl);
		code = N_NOT;
	    }
	    node_t n = c2mir_new_node1(c2m, code, operand);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}

	// Binary operators
	if (top && top->left && top->right) {
	    node_t left = cir_translate_expr(c2m, top->left);
	    node_t right = cir_translate_expr(c2m, top->right);

	    c2mir_node_code_t code;
	    switch (tb->id()) {
	    case TokenID::tkAdd:       code = N_ADD; break;
	    case TokenID::tkSub:       code = N_SUB; break;
	    case TokenID::tkMul:       code = N_MUL; break;
	    case TokenID::tkDiv:       code = N_DIV; break;
	    case TokenID::tkMod:       code = N_MOD; break;
	    case TokenID::tkAssign:    code = N_ASSIGN; break;
	    case TokenID::tkEquals:    code = N_EQ; break;
	    case TokenID::tkNotEq:     code = N_NE; break;
	    case TokenID::tkLT:        code = N_LT; break;
	    case TokenID::tkLE:        code = N_LE; break;
	    case TokenID::tkGT:        code = N_GT; break;
	    case TokenID::tkGE:        code = N_GE; break;
	    case TokenID::tkBand:      code = N_AND; break;
	    case TokenID::tkBor:       code = N_OR; break;
	    case TokenID::tkXor:       code = N_XOR; break;
	    case TokenID::tkLand:      code = N_ANDAND; break;
	    case TokenID::tkLor:       code = N_OROR; break;
	    case TokenID::tkBSL:       code = N_LSH; break;
	    case TokenID::tkBSR:       code = N_RSH; break;
	    default:
		DBG(std::cerr << "cir: unhandled binary op " << (int)tb->id() << std::endl);
		code = N_ADD;  // fallback
	    }

	    node_t n = c2mir_new_node2(c2m, code, left, right);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Function call
    if (tb->type() == TokenType::ttCallFunc) {
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(tb);
	if (tcf) {
	    node_t func_id = c2mir_new_str_node(c2m, N_ID, tcf->var.name.c_str(),
						tcf->var.name.size() + 1, pos);
	    node_t args = c2mir_new_node(c2m, N_LIST);
	    for (size_t i = 0; i < tcf->parameters.size(); i++) {
		c2mir_op_append(c2m, args, cir_translate_expr(c2m, tcf->parameters[i]));
	    }
	    node_t call = c2mir_new_node2(c2m, N_CALL, func_id, args);
	    c2mir_set_node_pos(c2m, call, pos);
	    return call;
	}
    }

    DBG(std::cerr << "cir: unhandled expr type=" << (int)tb->type()
		  << " id=" << (int)tb->id() << std::endl);
    return c2mir_new_i_node(c2m, 0, pos);  // fallback
}

// -----------------------------------------------------------------------
// Statement translation
// -----------------------------------------------------------------------

static node_t cir_translate_stmt(c2m_ctx_t c2m, TokenBase *tb);

static node_t cir_translate_return(c2m_ctx_t c2m, TokenRETURN *tr)
{
    c2mir_pos_t pos = make_pos(tr);
    node_t expr = tr->returns ? cir_translate_expr(c2m, tr->returns)
			      : c2mir_new_node(c2m, N_IGNORE);
    // c2mir RETURN has two children: LIST (empty for now) and the expression
    node_t ret = c2mir_new_node2(c2m, N_RETURN,
	c2mir_new_node(c2m, N_LIST), expr);
    c2mir_set_node_pos(c2m, ret, pos);
    return ret;
}

static node_t cir_translate_block(c2m_ctx_t c2m, TokenCpnd *tc)
{
    c2mir_pos_t pos = { tc->file, tc->line, tc->column };

    // c2mir BLOCK has two children: LIST (empty) and LIST (block items).
    // Both declarations and statements go in the second LIST.
    node_t empty_list = c2mir_new_node(c2m, N_LIST);
    node_t items = c2mir_new_node(c2m, N_LIST);

    // Emit local variable declarations first (skip function parameters).
    TokenFunc *parent_func = dynamic_cast<TokenFunc *>(tc);
    size_t skip = 0;
    if (parent_func) {
	FuncDef *fd = dynamic_cast<FuncDef *>(parent_func->var.type);
	if (fd) skip = fd->parameters.size();
    }

    for (size_t vi = skip; vi < tc->variables.size(); vi++) {
	Variable *v = tc->variables[vi];
	c2mir_pos_t vpos = pos;

	// SPEC_DECL(SHARE(LIST(type)), DECL(ID, LIST), IGNORE, IGNORE, IGNORE)
	node_t type_list = cir_type_list(c2m, v->type, vpos);
	node_t share = c2mir_new_node1(c2m, N_SHARE, type_list);

	node_t var_id = c2mir_new_str_node(c2m, N_ID, v->name.c_str(),
					   v->name.size() + 1, vpos);
	node_t var_decl = c2mir_new_node2(c2m, N_DECL,
	    var_id, c2mir_new_node(c2m, N_LIST));

	node_t spec_decl = c2mir_new_node(c2m, N_SPEC_DECL);
	c2mir_op_append(c2m, spec_decl, share);
	c2mir_op_append(c2m, spec_decl, var_decl);
	c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	c2mir_set_node_pos(c2m, spec_decl, vpos);

	c2mir_op_append(c2m, items, spec_decl);
    }

    // Then emit statements
    for (TokenStmt *ts : tc->statements) {
	node_t s = cir_translate_stmt(c2m, (TokenBase *)ts);
	if (s) c2mir_op_append(c2m, items, s);
    }
    c2mir_set_node_pos(c2m, items, pos);

    node_t block = c2mir_new_node2(c2m, N_BLOCK, empty_list, items);
    c2mir_set_node_pos(c2m, block, pos);
    return block;
}

static node_t cir_translate_if(c2m_ctx_t c2m, TokenIF *ti)
{
    c2mir_pos_t pos = make_pos(ti);
    node_t cond = cir_translate_expr(c2m, ti->condition);
    node_t then_body = cir_translate_stmt(c2m, ti->statement);
    node_t else_body = ti->elsestmt ? cir_translate_stmt(c2m, ti->elsestmt)
				    : c2mir_new_node(c2m, N_IGNORE);
    // c2mir IF: LIST(), condition, then_body, else_body
    node_t n = c2mir_new_node4(c2m, N_IF,
	c2mir_new_node(c2m, N_LIST), cond, then_body, else_body);
    c2mir_set_node_pos(c2m, n, pos);
    return n;
}

static node_t cir_translate_while(c2m_ctx_t c2m, TokenBase *tw)
{
    c2mir_pos_t pos = make_pos(tw);
    TokenWHILE *w = dynamic_cast<TokenWHILE *>(tw);
    if (!w) return c2mir_new_node(c2m, N_IGNORE);

    node_t cond = cir_translate_expr(c2m, w->condition);
    node_t body = cir_translate_stmt(c2m, w->statement);
    // c2mir WHILE: LIST(), condition, body
    node_t n = c2mir_new_node3(c2m, N_WHILE,
	c2mir_new_node(c2m, N_LIST), cond, body);
    c2mir_set_node_pos(c2m, n, pos);
    return n;
}

static node_t cir_translate_for(c2m_ctx_t c2m, TokenFOR *tf)
{
    c2mir_pos_t pos = make_pos(tf);
    node_t init = tf->initialize ? cir_translate_expr(c2m, tf->initialize)
				 : c2mir_new_node(c2m, N_IGNORE);
    node_t cond = tf->condition ? cir_translate_expr(c2m, tf->condition)
				: c2mir_new_node(c2m, N_IGNORE);
    node_t incr = tf->increment ? cir_translate_expr(c2m, tf->increment)
				: c2mir_new_node(c2m, N_IGNORE);
    node_t body = cir_translate_stmt(c2m, tf->statement);

    // c2mir FOR: LIST(), init_expr, cond_expr, incr_expr, body
    // Note: init/cond/incr are bare expressions (not wrapped in N_EXPR)
    node_t n = c2mir_new_node5(c2m, N_FOR,
	c2mir_new_node(c2m, N_LIST), init, cond, incr, body);
    c2mir_set_node_pos(c2m, n, pos);
    return n;
}

static node_t cir_translate_stmt(c2m_ctx_t c2m, TokenBase *tb)
{
    if (!tb) return NULL;

    // Return statement
    TokenRETURN *tr = dynamic_cast<TokenRETURN *>(tb);
    if (tr) return cir_translate_return(c2m, tr);

    // If statement
    TokenIF *ti = dynamic_cast<TokenIF *>(tb);
    if (ti) return cir_translate_if(c2m, ti);

    // While loop
    if (tb->id() == TokenID::tkWHILE)
	return cir_translate_while(c2m, tb);

    // For loop
    TokenFOR *tf = dynamic_cast<TokenFOR *>(tb);
    if (tf) return cir_translate_for(c2m, tf);

    // Compound statement (block)
    TokenCpnd *tc = dynamic_cast<TokenCpnd *>(tb);
    if (tc) return cir_translate_block(c2m, tc);

    // Expression statement (any expression followed by ;)
    // c2mir's N_EXPR has two children: LIST() and the expression
    node_t expr = cir_translate_expr(c2m, tb);
    node_t stmt = c2mir_new_node2(c2m, N_EXPR,
	c2mir_new_node(c2m, N_LIST), expr);
    c2mir_set_node_pos(c2m, stmt, make_pos(tb));
    return stmt;
}

// -----------------------------------------------------------------------
// Function forward declaration (prototype)
// -----------------------------------------------------------------------

static node_t cir_func_proto(c2m_ctx_t c2m, TokenFunc *tf)
{
    c2mir_pos_t pos = { tf->file, tf->line, tf->column };
    FuncDef *fd = dynamic_cast<FuncDef *>(tf->var.type);
    if (!fd) return NULL;

    // Return type
    node_t ret_type = cir_type_list(c2m, &fd->returns, pos);
    node_t share = c2mir_new_node1(c2m, N_SHARE, ret_type);

    // Parameters
    node_t param_list = c2mir_new_node(c2m, N_LIST);
    if (fd->parameters.empty()) {
	node_t void_spec = c2mir_new_node1(c2m, N_LIST, c2mir_new_node(c2m, N_VOID));
	c2mir_set_node_pos(c2m, void_spec, pos);
	node_t void_decl = c2mir_new_node2(c2m, N_DECL,
	    c2mir_new_node(c2m, N_IGNORE), c2mir_new_node(c2m, N_LIST));
	node_t void_param = c2mir_new_node2(c2m, N_TYPE, void_spec, void_decl);
	c2mir_set_node_pos(c2m, void_param, pos);
	c2mir_op_append(c2m, param_list, void_param);
    } else {
	for (size_t i = 0; i < fd->parameters.size(); i++) {
	    DataDef *ptype = fd->parameters[i];
	    const char *pname = "p";
	    if (tf->method && i < tf->method->parameters.size())
		pname = tf->method->parameters[i]->name.c_str();

	    node_t pspec = cir_type_list(c2m, ptype, pos);
	    node_t pid = c2mir_new_str_node(c2m, N_ID, pname, strlen(pname) + 1, pos);
	    node_t pdecl = c2mir_new_node2(c2m, N_DECL, pid, c2mir_new_node(c2m, N_LIST));
	    node_t spec_decl = c2mir_new_node(c2m, N_SPEC_DECL);
	    c2mir_op_append(c2m, spec_decl, pspec);
	    c2mir_op_append(c2m, spec_decl, pdecl);
	    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	    c2mir_set_node_pos(c2m, spec_decl, pos);
	    c2mir_op_append(c2m, param_list, spec_decl);
	}
    }
    c2mir_set_node_pos(c2m, param_list, pos);

    // Function declarator
    node_t func_inner = c2mir_new_node1(c2m, N_FUNC, param_list);
    c2mir_set_node_pos(c2m, func_inner, pos);

    // DECL(ID("name"), LIST(FUNC(...)))
    node_t func_id = c2mir_new_str_node(c2m, N_ID, tf->var.name.c_str(),
					tf->var.name.size() + 1, pos);
    node_t decl_list = c2mir_new_node1(c2m, N_LIST, func_inner);
    c2mir_set_node_pos(c2m, decl_list, pos);
    node_t decl = c2mir_new_node2(c2m, N_DECL, func_id, decl_list);
    c2mir_set_node_pos(c2m, decl, pos);

    // SPEC_DECL(SHARE(ret_type), DECL, IGNORE, IGNORE, IGNORE)
    node_t proto = c2mir_new_node(c2m, N_SPEC_DECL);
    c2mir_op_append(c2m, proto, share);
    c2mir_op_append(c2m, proto, decl);
    c2mir_op_append(c2m, proto, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, proto, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, proto, c2mir_new_node(c2m, N_IGNORE));
    c2mir_set_node_pos(c2m, proto, pos);

    return proto;
}

// -----------------------------------------------------------------------
// Function translation
// -----------------------------------------------------------------------

static node_t cir_translate_func(c2m_ctx_t c2m, TokenFunc *tf)
{
    c2mir_pos_t pos = { tf->file, tf->line, tf->column };
    FuncDef *fd = dynamic_cast<FuncDef *>(tf->var.type);
    if (!fd) return NULL;

    // Return type: LIST(type_spec)
    node_t ret_type = cir_type_list(c2m, &fd->returns, pos);

    // Parameters: LIST(TYPE(spec_list, DECL(name, LIST())) ...)
    node_t param_list = c2mir_new_node(c2m, N_LIST);

    if (fd->parameters.empty()) {
	// void parameter: TYPE(LIST(VOID), DECL(IGNORE, LIST()))
	node_t void_spec = c2mir_new_node1(c2m, N_LIST,
	    c2mir_new_node(c2m, N_VOID));
	c2mir_set_node_pos(c2m, void_spec, pos);
	node_t void_decl = c2mir_new_node2(c2m, N_DECL,
	    c2mir_new_node(c2m, N_IGNORE),
	    c2mir_new_node(c2m, N_LIST));
	node_t void_param = c2mir_new_node2(c2m, N_TYPE,
	    void_spec, void_decl);
	c2mir_set_node_pos(c2m, void_param, pos);
	c2mir_op_append(c2m, param_list, void_param);
    } else {
	// Named parameters
	for (size_t i = 0; i < fd->parameters.size(); i++) {
	    DataDef *ptype = fd->parameters[i];
	    // Get parameter name from the function's variable list
	    // Parameter names live in the Method object, not tf->variables
	    const char *pname = "p";
	    if (tf->method && i < tf->method->parameters.size())
		pname = tf->method->parameters[i]->name.c_str();
	    DBG(std::cerr << "cir: func " << tf->var.name << " param " << i
			  << " name='" << pname << "' vars.size=" << tf->variables.size()
			  << " params.size=" << fd->parameters.size() << std::endl);

	    node_t pspec = cir_type_list(c2m, ptype, pos);
	    node_t pid = c2mir_new_str_node(c2m, N_ID, pname,
					    strlen(pname) + 1, pos);
	    node_t pdecl = c2mir_new_node2(c2m, N_DECL,
		pid, c2mir_new_node(c2m, N_LIST));

	    // SPEC_DECL(type_list, decl, ignore, ignore, ignore)
	    node_t spec_decl = c2mir_new_node(c2m, N_SPEC_DECL);
	    c2mir_op_append(c2m, spec_decl, pspec);
	    c2mir_op_append(c2m, spec_decl, pdecl);
	    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
	    c2mir_set_node_pos(c2m, spec_decl, pos);

	    c2mir_op_append(c2m, param_list, spec_decl);
	}
    }
    c2mir_set_node_pos(c2m, param_list, pos);

    // Function declarator: FUNC(param_list)
    node_t func_inner = c2mir_new_node1(c2m, N_FUNC, param_list);
    c2mir_set_node_pos(c2m, func_inner, pos);

    // Declarator: DECL(ID("name"), LIST(FUNC(...)))
    node_t func_id = c2mir_new_str_node(c2m, N_ID, tf->var.name.c_str(),
					tf->var.name.size() + 1, pos);
    node_t decl_list = c2mir_new_node1(c2m, N_LIST, func_inner);
    c2mir_set_node_pos(c2m, decl_list, pos);
    node_t decl = c2mir_new_node2(c2m, N_DECL, func_id, decl_list);
    c2mir_set_node_pos(c2m, decl, pos);

    // Body: translate compound statement
    node_t body = cir_translate_block(c2m, (TokenCpnd *)tf);

    // FUNC_DEF(ret_type, decl, kr_decl_list, body)
    node_t func_def = c2mir_new_node4(c2m, N_FUNC_DEF,
	ret_type, decl,
	c2mir_new_node(c2m, N_LIST),  // K&R decl list (empty)
	body);
    c2mir_set_node_pos(c2m, func_def, pos);

    return func_def;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

c2m_ctx_t cir_init(MIR_context_t mir_ctx)
{
    struct c2mir_options *opts = new struct c2mir_options();
    memset(opts, 0, sizeof(*opts));
    opts->message_file = stderr;
    // opts->debug_p = 1;  // enable for AST dump after check
    return c2mir_init_compile(mir_ctx, opts);
}

node_t cir_translate(c2m_ctx_t c2m, Program *prog)
{
    if (!prog) return NULL;

    node_t module = c2mir_new_node(c2m, N_MODULE);
    // c2mir expects MODULE to contain a single LIST child.
    // The checker and generator both process NL_HEAD(module->u.ops).
    node_t top_list = c2mir_new_node(c2m, N_LIST);

    // Collect all TokenFunc entries
    std::vector<TokenFunc *> funcs;
    for (auto it = prog->pending_funcs.begin();
	 it != prog->pending_funcs.end(); ++it) {
	TokenFunc *tf = dynamic_cast<TokenFunc *>(*it);
	if (tf && !tf->is_overridden)
	    funcs.push_back(tf);
    }

    // Pass 1: Emit forward declarations for all functions except main.
    for (TokenFunc *tf : funcs) {
	if (tf->var.name == "main") continue;
	node_t proto = cir_func_proto(c2m, tf);
	if (proto) c2mir_op_append(c2m, top_list, proto);
    }

    // Pass 2: Emit function definitions.
    for (TokenFunc *tf : funcs) {
	node_t fd = cir_translate_func(c2m, tf);
	if (fd) c2mir_op_append(c2m, top_list, fd);
    }

    c2mir_op_append(c2m, module, top_list);
    return module;
}

int cir_compile(MIR_context_t mir_ctx, c2m_ctx_t c2m, node_t tree,
		const char *module_name)
{
    return c2mir_compile_tree(mir_ctx, c2m, tree, module_name);
}

void cir_finish(c2m_ctx_t c2m)
{
    c2mir_finish_compile(c2m);
}
