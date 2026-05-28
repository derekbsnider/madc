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
#include <dlfcn.h>

#include <asmjit/x86.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_cir.h"

extern "C" {
#include "c2mir/c2mir_api.h"
#include "mir-gen.h"
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

// Append type specifier node(s) into a LIST for the given DataDef.
// Handles multi-specifier types (unsigned int, long long, etc.)
static void cir_append_type_specs(c2m_ctx_t c2m, node_t list, DataDef *dd)
{
    if (!dd) { c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_INT)); return; }

    DataType dt = dd->rawtype();
    switch (dt) {
    case DataType::dtVOID:   c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_VOID)); break;
    case DataType::dtCHAR:   c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_CHAR)); break;
    case DataType::dtINT16:  c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_SHORT)); break;
    case DataType::dtINT32:  c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_INT)); break;
    case DataType::dtINT64:
	// long on x86-64 is 64-bit
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_LONG));
	break;
    case DataType::dtUINT8:
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_UNSIGNED));
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_CHAR));
	break;
    case DataType::dtUINT16:
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_UNSIGNED));
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_SHORT));
	break;
    case DataType::dtUINT32:
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_UNSIGNED));
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_INT));
	break;
    case DataType::dtUINT64:
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_UNSIGNED));
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_LONG));
	break;
    case DataType::dtFLOAT:  c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_FLOAT)); break;
    case DataType::dtDOUBLE: c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_DOUBLE)); break;
    default:
	c2mir_op_append(c2m, list, c2mir_new_node(c2m, N_INT));
    }
}

// Single-node type specifier for simple types (used by cir_struct_def, etc.)
static node_t cir_type_spec(c2m_ctx_t c2m, DataDef *dd)
{
    if (!dd) return c2mir_new_node(c2m, N_INT);
    DataType dt = dd->rawtype();
    switch (dt) {
    case DataType::dtVOID:   return c2mir_new_node(c2m, N_VOID);
    case DataType::dtCHAR:   return c2mir_new_node(c2m, N_CHAR);
    case DataType::dtINT16:  return c2mir_new_node(c2m, N_SHORT);
    case DataType::dtINT32:  return c2mir_new_node(c2m, N_INT);
    case DataType::dtINT64:  return c2mir_new_node(c2m, N_LONG);
    case DataType::dtFLOAT:  return c2mir_new_node(c2m, N_FLOAT);
    case DataType::dtDOUBLE: return c2mir_new_node(c2m, N_DOUBLE);
    default:                 return c2mir_new_node(c2m, N_INT);
    }
}

// Build a type specifier LIST node from a DataDef.
// Returns N_LIST(type_spec, ...) matching c2mir's declaration_specifiers.
static node_t cir_type_list(c2m_ctx_t c2m, DataDef *dd, c2mir_pos_t pos)
{
    node_t list = c2mir_new_node(c2m, N_LIST);

    // Struct types: LIST(STRUCT(ID("name"), IGNORE))
    if (dd && dd->is_struct()) {
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
	if (sdd) {
	    node_t sid = c2mir_new_str_node(c2m, N_ID, sdd->name.c_str(),
					    sdd->name.size() + 1, pos);
	    node_t struct_ref = c2mir_new_node2(c2m, N_STRUCT, sid,
		c2mir_new_node(c2m, N_IGNORE));
	    c2mir_set_node_pos(c2m, struct_ref, pos);
	    c2mir_op_append(c2m, list, struct_ref);
	    c2mir_set_node_pos(c2m, list, pos);
	    return list;
	}
    }

    cir_append_type_specs(c2m, list, dd);
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
	    // Constant integer variables (enum values) → emit literal value
	    // But NOT string literals or other non-integer constants.
	    if (tv->var.is_constant() && tv->var.type &&
		tv->var.type->is_integer() && !tv->var.type->is_pointer()) {
		return c2mir_new_i_node(c2m, tv->var.get<int64_t>(), pos);
	    }
	    // String literal variables: prefixed with "__literal__" by parser.
	    // Extract the actual string content and emit as N_STR.
	    if (tv->var.name.compare(0, 11, "__literal__") == 0) {
		const std::string &content = tv->var.name.substr(11);
		return c2mir_new_str_node(c2m, N_STR, content.c_str(),
					 content.size() + 1, pos);
	    }
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

    // Ternary operator: cond ? true_expr : false_expr → COND(cond, true, false)
    {
	TokenTerQ *tq = dynamic_cast<TokenTerQ *>(tb);
	if (tq) {
	    node_t cond = cir_translate_expr(c2m, tq->condition);
	    node_t t = cir_translate_expr(c2m, tq->true_expr);
	    node_t f = cir_translate_expr(c2m, tq->false_expr);
	    node_t n = c2mir_new_node3(c2m, N_COND, cond, t, f);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Address-of: &var → ADDR(ID)
    {
	TokenAddrOf *ta = dynamic_cast<TokenAddrOf *>(tb);
	if (ta) {
	    node_t id = c2mir_new_str_node(c2m, N_ID, ta->var.name.c_str(),
					   ta->var.name.size() + 1, pos);
	    node_t n = c2mir_new_node1(c2m, N_ADDR, id);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Address-of expression: &(expr) → ADDR(expr)
    {
	TokenAddrExpr *tae = dynamic_cast<TokenAddrExpr *>(tb);
	if (tae) {
	    node_t expr = cir_translate_expr(c2m, tae->expr);
	    node_t n = c2mir_new_node1(c2m, N_ADDR, expr);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Dereference: *ptr → DEREF(ID)
    {
	TokenDeref *td = dynamic_cast<TokenDeref *>(tb);
	if (td) {
	    node_t id = c2mir_new_str_node(c2m, N_ID, td->var.name.c_str(),
					   td->var.name.size() + 1, pos);
	    node_t n = c2mir_new_node1(c2m, N_DEREF, id);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Dereference expression: *(expr) → DEREF(expr)
    {
	TokenDerefExpr *tde = dynamic_cast<TokenDerefExpr *>(tb);
	if (tde) {
	    node_t expr = cir_translate_expr(c2m, tde->expr);
	    node_t n = c2mir_new_node1(c2m, N_DEREF, expr);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Array subscript: arr[i] → IND(ID, index)
    {
	TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(tb);
	if (tsub) {
	    node_t base = c2mir_new_str_node(c2m, N_ID,
		tsub->object.name.c_str(), tsub->object.name.size() + 1, pos);
	    node_t idx = cir_translate_expr(c2m, tsub->index);
	    node_t n = c2mir_new_node2(c2m, N_IND, base, idx);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Struct member access: obj.member → FIELD(ID, ID)
    {
	TokenMember *tm = dynamic_cast<TokenMember *>(tb);
	if (tm) {
	    node_t obj;
	    if (tm->parent_expr) {
		obj = cir_translate_expr(c2m, tm->parent_expr);
	    } else {
		obj = c2mir_new_str_node(c2m, N_ID,
		    tm->object.name.c_str(), tm->object.name.size() + 1, pos);
	    }
	    node_t member = c2mir_new_str_node(c2m, N_ID,
		tm->var.name.c_str(), tm->var.name.size() + 1, pos);
	    // Use DEREF_FIELD for pointer->member, FIELD for obj.member
	    c2mir_node_code_t code = tm->object.type->is_pointer() ? N_DEREF_FIELD : N_FIELD;
	    node_t n = c2mir_new_node2(c2m, code, obj, member);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // sizeof(type) → SIZEOF(TYPE(LIST(type_spec), DECL(IGNORE, LIST())))
    {
	TokenTypeQuery *ttq = dynamic_cast<TokenTypeQuery *>(tb);
	if (ttq && ttq->query_type) {
	    node_t type_list = cir_type_list(c2m, ttq->query_type, pos);
	    node_t type_decl = c2mir_new_node2(c2m, N_DECL,
		c2mir_new_node(c2m, N_IGNORE), c2mir_new_node(c2m, N_LIST));
	    node_t type_node = c2mir_new_node2(c2m, N_TYPE, type_list, type_decl);
	    c2mir_set_node_pos(c2m, type_node, pos);
	    c2mir_node_code_t code = ttq->want_alignof ? N_ALIGNOF : N_SIZEOF;
	    node_t n = c2mir_new_node1(c2m, code, type_node);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Cast: (type)expr → CAST(TYPE(LIST(type_spec), DECL(IGNORE, LIST([POINTER]))), expr)
    {
	TokenCast *tc = dynamic_cast<TokenCast *>(tb);
	if (tc) {
	    // Unwrap pointer for the type specifier
	    DataDef *cast_dd = tc->cast_type;
	    bool cast_is_ptr = cast_dd && cast_dd->is_pointer();
	    DataDefPTR *cast_ptr = cast_is_ptr ? dynamic_cast<DataDefPTR *>(cast_dd) : nullptr;
	    if (cast_ptr && cast_ptr->base_type) cast_dd = cast_ptr->base_type;

	    node_t type_list = cir_type_list(c2m, cast_dd, pos);
	    node_t cast_decl_list = c2mir_new_node(c2m, N_LIST);
	    if (cast_is_ptr) {
		node_t pointer = c2mir_new_node1(c2m, N_POINTER, c2mir_new_node(c2m, N_LIST));
		c2mir_set_node_pos(c2m, pointer, pos);
		c2mir_op_append(c2m, cast_decl_list, pointer);
	    }
	    node_t type_decl = c2mir_new_node2(c2m, N_DECL,
		c2mir_new_node(c2m, N_IGNORE), cast_decl_list);
	    node_t type_node = c2mir_new_node2(c2m, N_TYPE, type_list, type_decl);
	    c2mir_set_node_pos(c2m, type_node, pos);
	    node_t expr = cir_translate_expr(c2m, tc->expr);
	    node_t n = c2mir_new_node2(c2m, N_CAST, type_node, expr);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Operators (unary and binary)
    if (tb->is_operator()) {
	TokenOperator *top = dynamic_cast<TokenOperator *>(tb);

	// Increment/decrement
	if (tb->id() == TokenID::tkInc || tb->id() == TokenID::tkDec) {
	    // left set = postfix (x++), right set = prefix (++x)
	    bool is_post = (top->left != nullptr);
	    TokenBase *operand_tb = is_post ? top->left : top->right;
	    node_t operand = cir_translate_expr(c2m, operand_tb);
	    c2mir_node_code_t code;
	    if (tb->id() == TokenID::tkInc)
		code = is_post ? N_POST_INC : N_INC;
	    else
		code = is_post ? N_POST_DEC : N_DEC;
	    node_t n = c2mir_new_node1(c2m, code, operand);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}

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

	// Binary operators (including compound assignment)
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
	    case TokenID::tkComma:     code = N_COMMA; break;
	    // Compound assignment
	    case TokenID::tkAddEq:     code = N_ADD_ASSIGN; break;
	    case TokenID::tkSubEq:     code = N_SUB_ASSIGN; break;
	    case TokenID::tkMulEq:     code = N_MUL_ASSIGN; break;
	    case TokenID::tkDivEq:     code = N_DIV_ASSIGN; break;
	    case TokenID::tkModEq:     code = N_MOD_ASSIGN; break;
	    case TokenID::tkBandEq:    code = N_AND_ASSIGN; break;
	    case TokenID::tkBorEq:     code = N_OR_ASSIGN; break;
	    case TokenID::tkXorEq:     code = N_XOR_ASSIGN; break;
	    case TokenID::tkBSLEq:     code = N_LSH_ASSIGN; break;
	    case TokenID::tkBSREq:     code = N_RSH_ASSIGN; break;
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

// Build a SPEC_DECL for a variable declaration, handling pointer and array types,
// storage class qualifiers, and brace initializers.
static node_t cir_var_decl(c2m_ctx_t c2m, Variable *v, c2mir_pos_t vpos,
			    TokenDecl *tdecl = nullptr)
{
    // Determine base type — for pointers, use the pointed-to type
    DataDef *base_dd = v->type;
    bool is_ptr = base_dd && base_dd->is_pointer();
    DataDefPTR *ptr_dd = is_ptr ? dynamic_cast<DataDefPTR *>(base_dd) : nullptr;
    if (ptr_dd && ptr_dd->base_type)
	base_dd = ptr_dd->base_type;

    node_t type_list = cir_type_list(c2m, base_dd, vpos);

    // Add storage class qualifiers to the type list
    if (v->flags & vfSTATIC) {
	node_t stat = c2mir_new_node(c2m, N_STATIC);
	c2mir_set_node_pos(c2m, stat, vpos);
	// Prepend STATIC before type specifiers — rebuild the list
	node_t new_list = c2mir_new_node(c2m, N_LIST);
	c2mir_op_append(c2m, new_list, stat);
	// Copy existing children (type specifiers)
	// Since we can't iterate c2mir node children easily,
	// just rebuild: STATIC goes in the same LIST as the type
	// Actually, cir_type_list returns LIST(type_spec). We need
	// LIST(STATIC, type_spec). Simplest: insert into existing list.
	// c2mir nodes use DLIST for children. We can append STATIC then type.
	// But we already built type_list. Let's just rebuild.
	node_t spec = cir_type_spec(c2m, base_dd);
	if (base_dd && base_dd->is_struct()) {
	    DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
	    if (sdd) {
		node_t sid = c2mir_new_str_node(c2m, N_ID, sdd->name.c_str(),
						sdd->name.size() + 1, vpos);
		spec = c2mir_new_node2(c2m, N_STRUCT, sid, c2mir_new_node(c2m, N_IGNORE));
		c2mir_set_node_pos(c2m, spec, vpos);
	    }
	}
	c2mir_op_append(c2m, new_list, spec);
	c2mir_set_node_pos(c2m, new_list, vpos);
	type_list = new_list;
    }
    if (v->flags & vfEXTERN) {
	node_t ext = c2mir_new_node(c2m, N_EXTERN);
	c2mir_set_node_pos(c2m, ext, vpos);
	node_t new_list = c2mir_new_node(c2m, N_LIST);
	c2mir_op_append(c2m, new_list, ext);
	node_t spec = cir_type_spec(c2m, base_dd);
	c2mir_op_append(c2m, new_list, spec);
	c2mir_set_node_pos(c2m, new_list, vpos);
	type_list = new_list;
    }

    node_t share = c2mir_new_node1(c2m, N_SHARE, type_list);

    node_t var_id = c2mir_new_str_node(c2m, N_ID, v->name.c_str(),
				       v->name.size() + 1, vpos);
    // Build declarator LIST — may contain POINTER or ARR nodes
    node_t decl_list = c2mir_new_node(c2m, N_LIST);

    if (is_ptr) {
	// POINTER(LIST()) — pointer to base type
	node_t ptr_quals = c2mir_new_node(c2m, N_LIST);
	node_t pointer = c2mir_new_node1(c2m, N_POINTER, ptr_quals);
	c2mir_set_node_pos(c2m, pointer, vpos);
	c2mir_op_append(c2m, decl_list, pointer);
    }

    if (v->is_fixed_array() && !v->dims.empty()) {
	// ARR(IGNORE, LIST(), size) — fixed-size array
	node_t size = c2mir_new_i_node(c2m, v->dims[0], vpos);
	node_t arr = c2mir_new_node3(c2m, N_ARR,
	    c2mir_new_node(c2m, N_IGNORE),
	    c2mir_new_node(c2m, N_LIST),
	    size);
	c2mir_set_node_pos(c2m, arr, vpos);
	c2mir_op_append(c2m, decl_list, arr);
    }

    node_t var_decl = c2mir_new_node2(c2m, N_DECL, var_id, decl_list);

    // Build initializer (5th child of SPEC_DECL)
    node_t init_node = c2mir_new_node(c2m, N_IGNORE);
    if (tdecl && tdecl->has_brace_init && !tdecl->init_list.empty()) {
	// Brace initializer: LIST(INIT(LIST(), val), INIT(LIST(), val), ...)
	init_node = c2mir_new_node(c2m, N_LIST);
	for (auto *elem : tdecl->init_list) {
	    node_t val = cir_translate_expr(c2m, elem);
	    node_t init = c2mir_new_node2(c2m, N_INIT,
		c2mir_new_node(c2m, N_LIST), val);
	    c2mir_set_node_pos(c2m, init, vpos);
	    c2mir_op_append(c2m, init_node, init);
	}
	c2mir_set_node_pos(c2m, init_node, vpos);
    } else if (tdecl && tdecl->initialize) {
	// Scalar initializer
	init_node = cir_translate_expr(c2m, tdecl->initialize);
    }

    node_t spec_decl = c2mir_new_node(c2m, N_SPEC_DECL);
    c2mir_op_append(c2m, spec_decl, share);
    c2mir_op_append(c2m, spec_decl, var_decl);
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, spec_decl, init_node);
    c2mir_set_node_pos(c2m, spec_decl, vpos);

    return spec_decl;
}

// Build a struct definition SPEC_DECL for the top-level declarations list.
static node_t cir_struct_def(c2m_ctx_t c2m, DataDefSTRUCT *sdd, c2mir_pos_t pos)
{
    // STRUCT(ID("name"), LIST(MEMBER, MEMBER, ...))
    node_t struct_id = c2mir_new_str_node(c2m, N_ID, sdd->name.c_str(),
					  sdd->name.size() + 1, pos);
    node_t member_list = c2mir_new_node(c2m, N_LIST);

    for (size_t i = 0; i < sdd->members.size(); i++) {
	DataDef *mtype = sdd->members[i].second;
	const std::string &mname = sdd->members[i].first;

	// Unwrap pointer types for the type specifier
	DataDef *mbase = mtype;
	bool m_is_ptr = mbase && mbase->is_pointer();
	DataDefPTR *mptr = m_is_ptr ? dynamic_cast<DataDefPTR *>(mbase) : nullptr;
	if (mptr && mptr->base_type) mbase = mptr->base_type;

	node_t mspec = cir_type_list(c2m, mbase, pos);
	node_t mshare = c2mir_new_node1(c2m, N_SHARE, mspec);
	node_t mid = c2mir_new_str_node(c2m, N_ID, mname.c_str(),
					mname.size() + 1, pos);
	node_t mdecl_list = c2mir_new_node(c2m, N_LIST);
	if (m_is_ptr) {
	    node_t pointer = c2mir_new_node1(c2m, N_POINTER, c2mir_new_node(c2m, N_LIST));
	    c2mir_set_node_pos(c2m, pointer, pos);
	    c2mir_op_append(c2m, mdecl_list, pointer);
	}
	node_t mdecl = c2mir_new_node2(c2m, N_DECL, mid, mdecl_list);
	// MEMBER has 4 children: SHARE(type), DECL, IGNORE, IGNORE
	node_t member = c2mir_new_node(c2m, N_MEMBER);
	c2mir_op_append(c2m, member, mshare);
	c2mir_op_append(c2m, member, mdecl);
	c2mir_op_append(c2m, member, c2mir_new_node(c2m, N_IGNORE));
	c2mir_op_append(c2m, member, c2mir_new_node(c2m, N_IGNORE));
	c2mir_set_node_pos(c2m, member, pos);
	c2mir_op_append(c2m, member_list, member);
    }

    node_t struct_node = c2mir_new_node2(c2m, N_STRUCT, struct_id, member_list);
    c2mir_set_node_pos(c2m, struct_node, pos);

    // Wrap in SPEC_DECL: LIST(STRUCT(...)), IGNORE*4
    node_t type_list = c2mir_new_node1(c2m, N_LIST, struct_node);
    c2mir_set_node_pos(c2m, type_list, pos);

    node_t spec_decl = c2mir_new_node(c2m, N_SPEC_DECL);
    c2mir_op_append(c2m, spec_decl, type_list);
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_set_node_pos(c2m, spec_decl, pos);

    return spec_decl;
}

// Build a SPEC_DECL for a function parameter, handling pointer types.
static node_t cir_param_decl(c2m_ctx_t c2m, DataDef *ptype, const char *pname,
			     c2mir_pos_t pos)
{
    // Determine base type — for pointers, use the pointed-to type
    DataDef *base_dd = ptype;
    bool is_ptr = base_dd && base_dd->is_pointer();
    DataDefPTR *ptr_dd = is_ptr ? dynamic_cast<DataDefPTR *>(base_dd) : nullptr;
    if (ptr_dd && ptr_dd->base_type)
	base_dd = ptr_dd->base_type;

    node_t pspec = cir_type_list(c2m, base_dd, pos);
    node_t pid = c2mir_new_str_node(c2m, N_ID, pname, strlen(pname) + 1, pos);

    node_t pdecl_list = c2mir_new_node(c2m, N_LIST);
    if (is_ptr) {
	node_t pointer = c2mir_new_node1(c2m, N_POINTER, c2mir_new_node(c2m, N_LIST));
	c2mir_set_node_pos(c2m, pointer, pos);
	c2mir_op_append(c2m, pdecl_list, pointer);
    }
    node_t pdecl = c2mir_new_node2(c2m, N_DECL, pid, pdecl_list);

    node_t spec_decl = c2mir_new_node(c2m, N_SPEC_DECL);
    c2mir_op_append(c2m, spec_decl, pspec);
    c2mir_op_append(c2m, spec_decl, pdecl);
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_op_append(c2m, spec_decl, c2mir_new_node(c2m, N_IGNORE));
    c2mir_set_node_pos(c2m, spec_decl, pos);
    return spec_decl;
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

    // Collect names of variables that have TokenDecl statements (with initializers).
    // Those will be emitted in the statement pass instead.
    // Note: statements vector stores TokenStmt* but actual objects may be TokenDecl
    // (which doesn't extend TokenStmt), so cast through TokenBase* first.
    std::set<std::string> decl_vars;
    for (auto *ts : tc->statements) {
	TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)ts);
	if (td)
	    decl_vars.insert(td->var.name);
    }

    DBG(std::cerr << "cir: block vars=" << tc->variables.size()
		  << " skip=" << skip << " stmts=" << tc->statements.size()
		  << " decl_vars=" << decl_vars.size() << std::endl);
    for (size_t vi = skip; vi < tc->variables.size(); vi++) {
	Variable *v = tc->variables[vi];
	DBG(std::cerr << "cir:   var[" << vi << "] '" << v->name
		      << "' type=" << (v->type ? v->type->name : "null")
		      << " skip_decl=" << decl_vars.count(v->name) << std::endl);
	if (decl_vars.count(v->name)) continue;  // emitted with initializer later
	c2mir_pos_t vpos = pos;
	node_t spec_decl = cir_var_decl(c2m, v, vpos);
	c2mir_op_append(c2m, items, spec_decl);
    }

    // Then emit statements, handling labels.
    // Labels attach to the NEXT statement's label LIST in c2mir.
    std::vector<std::string> pending_labels;
    for (size_t si = 0; si < tc->statements.size(); si++) {
	TokenBase *stb = (TokenBase *)tc->statements[si];

	// Collect labels
	TokenLabel *tl = dynamic_cast<TokenLabel *>(stb);
	if (tl) {
	    pending_labels.push_back(tl->name);
	    continue;
	}

	node_t s = cir_translate_stmt(c2m, stb);
	if (!s) continue;

	// If we have pending labels, inject them into the statement.
	// c2mir statements have a label LIST as their first child.
	// We can't easily replace children in a built node, so we
	// insert a labeled empty expression BEFORE the actual statement.
	if (!pending_labels.empty()) {
	    node_t label_list = c2mir_new_node(c2m, N_LIST);
	    for (auto &lname : pending_labels) {
		node_t lid = c2mir_new_str_node(c2m, N_ID, lname.c_str(),
						lname.size() + 1, pos);
		node_t label = c2mir_new_node1(c2m, N_LABEL, lid);
		c2mir_set_node_pos(c2m, label, pos);
		c2mir_op_append(c2m, label_list, label);
	    }
	    pending_labels.clear();

	    // Emit a labeled no-op before the actual statement
	    node_t labeled_nop = c2mir_new_node2(c2m, N_EXPR,
		label_list, c2mir_new_i_node(c2m, 0, pos));
	    c2mir_set_node_pos(c2m, labeled_nop, pos);
	    c2mir_op_append(c2m, items, labeled_nop);
	}

	c2mir_op_append(c2m, items, s);
    }

    // If labels remain at end of block (label before closing }), emit empty stmt
    if (!pending_labels.empty()) {
	node_t label_list = c2mir_new_node(c2m, N_LIST);
	for (auto &lname : pending_labels) {
	    node_t lid = c2mir_new_str_node(c2m, N_ID, lname.c_str(),
					    lname.size() + 1, pos);
	    node_t label = c2mir_new_node1(c2m, N_LABEL, lid);
	    c2mir_set_node_pos(c2m, label, pos);
	    c2mir_op_append(c2m, label_list, label);
	}
	node_t empty = c2mir_new_node2(c2m, N_EXPR,
	    label_list, c2mir_new_i_node(c2m, 0, pos));
	c2mir_set_node_pos(c2m, empty, pos);
	c2mir_op_append(c2m, items, empty);
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

static node_t cir_translate_do(c2m_ctx_t c2m, TokenDO *td)
{
    c2mir_pos_t pos = make_pos(td);
    node_t cond = cir_translate_expr(c2m, td->condition);
    node_t body = cir_translate_stmt(c2m, td->statement);
    // c2mir DO: LIST(), condition, body
    node_t n = c2mir_new_node3(c2m, N_DO, c2mir_new_node(c2m, N_LIST), cond, body);
    c2mir_set_node_pos(c2m, n, pos);
    return n;
}

static node_t cir_translate_switch(c2m_ctx_t c2m, TokenSWITCH *ts)
{
    c2mir_pos_t pos = make_pos(ts);
    node_t expr = cir_translate_expr(c2m, ts->expression);

    // Build a BLOCK containing all case statements.
    // c2mir SWITCH: LIST(), expr, body_block
    // Inside the body, each statement's label LIST contains CASE or DEFAULT nodes.
    node_t block_items = c2mir_new_node(c2m, N_LIST);

    for (size_t ci = 0; ci < ts->cases.size(); ci++) {
	TokenCASE *tc = ts->cases[ci];
	bool is_default = (tc->value == NULL);

	// Build label for this case
	node_t label;
	if (is_default) {
	    label = c2mir_new_node(c2m, N_DEFAULT);
	    c2mir_set_node_pos(c2m, label, make_pos(tc));
	} else {
	    node_t val = cir_translate_expr(c2m, tc->value);
	    label = c2mir_new_node1(c2m, N_CASE, val);
	    c2mir_set_node_pos(c2m, label, make_pos(tc));
	}

	// Emit each statement in this case arm with the label on the first one
	for (size_t si = 0; si < tc->statements.size(); si++) {
	    node_t s = cir_translate_stmt(c2m, tc->statements[si]);
	    if (!s) continue;
	    if (si == 0) {
		// Attach the case/default label to the first statement's label list.
		// Statements are N_EXPR/N_RETURN/etc. — their first child is the label LIST.
		// Replace the empty LIST with one containing the label.
		// For simplicity, wrap in a labeled expression if needed.
		node_t label_list = c2mir_new_node1(c2m, N_LIST, label);
		c2mir_set_node_pos(c2m, label_list, make_pos(tc));
		// Replace s's first child (the empty label LIST) with our labeled one.
		// Since we can't easily replace children, rebuild the statement.
		// For N_RETURN and N_EXPR which have LIST as first child:
		// We need to inject the label. Use a wrapper approach.
		// Actually, the simplest approach: build a labeled expression statement.
		if (tc->statements.size() == 0) {
		    // Empty case: emit a labeled empty expression
		    node_t empty = c2mir_new_node2(c2m, N_EXPR,
			label_list, c2mir_new_i_node(c2m, 0, make_pos(tc)));
		    c2mir_set_node_pos(c2m, empty, make_pos(tc));
		    c2mir_op_append(c2m, block_items, empty);
		} else {
		    // Rebuild the statement with the label
		    // Peek at the statement type to know how to rebuild it
		    TokenRETURN *ret = dynamic_cast<TokenRETURN *>(tc->statements[si]);
		    if (ret) {
			node_t retexpr = ret->returns
			    ? cir_translate_expr(c2m, ret->returns)
			    : c2mir_new_node(c2m, N_IGNORE);
			node_t labeled_ret = c2mir_new_node2(c2m, N_RETURN,
			    label_list, retexpr);
			c2mir_set_node_pos(c2m, labeled_ret, make_pos(ret));
			c2mir_op_append(c2m, block_items, labeled_ret);
		    } else if (dynamic_cast<TokenBREAK *>(tc->statements[si])) {
			node_t brk = c2mir_new_node1(c2m, N_BREAK, label_list);
			c2mir_set_node_pos(c2m, brk, make_pos(tc->statements[si]));
			c2mir_op_append(c2m, block_items, brk);
		    } else {
			// General case: wrap as labeled expression statement
			node_t inner = cir_translate_expr(c2m, tc->statements[si]);
			node_t labeled_expr = c2mir_new_node2(c2m, N_EXPR,
			    label_list, inner);
			c2mir_set_node_pos(c2m, labeled_expr, make_pos(tc->statements[si]));
			c2mir_op_append(c2m, block_items, labeled_expr);
		    }
		}
	    } else {
		c2mir_op_append(c2m, block_items, s);
	    }
	}

	// If this case had no statements, emit an empty labeled expression
	if (tc->statements.empty()) {
	    node_t label_list = c2mir_new_node1(c2m, N_LIST, label);
	    node_t empty = c2mir_new_node2(c2m, N_EXPR,
		label_list, c2mir_new_i_node(c2m, 0, make_pos(tc)));
	    c2mir_set_node_pos(c2m, empty, make_pos(tc));
	    c2mir_op_append(c2m, block_items, empty);
	}
    }

    // Handle default case if not already in cases vector
    if (ts->defaultcase && ts->default_index < 0) {
	TokenCASE *dc = ts->defaultcase;
	node_t def_label = c2mir_new_node(c2m, N_DEFAULT);
	c2mir_set_node_pos(c2m, def_label, make_pos(dc));
	for (size_t si = 0; si < dc->statements.size(); si++) {
	    node_t s = cir_translate_stmt(c2m, dc->statements[si]);
	    if (!s) continue;
	    if (si == 0) {
		node_t label_list = c2mir_new_node1(c2m, N_LIST, def_label);
		TokenRETURN *ret = dynamic_cast<TokenRETURN *>(dc->statements[si]);
		if (ret) {
		    node_t retexpr = ret->returns
			? cir_translate_expr(c2m, ret->returns)
			: c2mir_new_node(c2m, N_IGNORE);
		    node_t labeled_ret = c2mir_new_node2(c2m, N_RETURN,
			label_list, retexpr);
		    c2mir_set_node_pos(c2m, labeled_ret, make_pos(ret));
		    c2mir_op_append(c2m, block_items, labeled_ret);
		} else {
		    node_t inner = cir_translate_expr(c2m, dc->statements[si]);
		    node_t labeled_expr = c2mir_new_node2(c2m, N_EXPR,
			label_list, inner);
		    c2mir_set_node_pos(c2m, labeled_expr, make_pos(dc->statements[si]));
		    c2mir_op_append(c2m, block_items, labeled_expr);
		}
	    } else {
		c2mir_op_append(c2m, block_items, s);
	    }
	}
    }

    node_t body = c2mir_new_node2(c2m, N_BLOCK,
	c2mir_new_node(c2m, N_LIST), block_items);
    c2mir_set_node_pos(c2m, body, pos);

    node_t n = c2mir_new_node3(c2m, N_SWITCH,
	c2mir_new_node(c2m, N_LIST), expr, body);
    c2mir_set_node_pos(c2m, n, pos);
    return n;
}

static node_t cir_translate_stmt(c2m_ctx_t c2m, TokenBase *tb)
{
    if (!tb) return NULL;

    c2mir_pos_t pos = make_pos(tb);

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

    // Do-while loop
    {
	TokenDO *td = dynamic_cast<TokenDO *>(tb);
	if (td) return cir_translate_do(c2m, td);
    }

    // Switch statement
    {
	TokenSWITCH *ts = dynamic_cast<TokenSWITCH *>(tb);
	if (ts) return cir_translate_switch(c2m, ts);
    }

    // Goto: GOTO(LIST(), ID("label"))
    {
	TokenGOTO *tg = dynamic_cast<TokenGOTO *>(tb);
	if (tg) {
	    node_t label_id = c2mir_new_str_node(c2m, N_ID, tg->target.c_str(),
						 tg->target.size() + 1, pos);
	    node_t n = c2mir_new_node2(c2m, N_GOTO,
		c2mir_new_node(c2m, N_LIST), label_id);
	    c2mir_set_node_pos(c2m, n, pos);
	    return n;
	}
    }

    // Label: handled in cir_translate_block (attaches to next statement)
    {
	TokenLabel *tl = dynamic_cast<TokenLabel *>(tb);
	if (tl) return NULL;
    }

    // Declaration statement: emit as SPEC_DECL (with optional initializer)
    {
	TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)tb);
	if (td) {
	    return cir_var_decl(c2m, &td->var, pos, td);
	}
    }

    // Break
    if (tb->id() == TokenID::tkBREAK) {
	node_t n = c2mir_new_node1(c2m, N_BREAK, c2mir_new_node(c2m, N_LIST));
	c2mir_set_node_pos(c2m, n, pos);
	return n;
    }

    // Continue
    if (tb->id() == TokenID::tkCONT) {
	node_t n = c2mir_new_node1(c2m, N_CONTINUE, c2mir_new_node(c2m, N_LIST));
	c2mir_set_node_pos(c2m, n, pos);
	return n;
    }

    // Compound statement (block)
    TokenCpnd *tc = dynamic_cast<TokenCpnd *>(tb);
    if (tc) return cir_translate_block(c2m, tc);

    // Expression statement (any expression followed by ;)
    // c2mir's N_EXPR has two children: LIST() and the expression
    node_t expr = cir_translate_expr(c2m, tb);
    node_t stmt = c2mir_new_node2(c2m, N_EXPR,
	c2mir_new_node(c2m, N_LIST), expr);
    c2mir_set_node_pos(c2m, stmt, pos);
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

    // Return type — unwrap pointer if needed
    DataDef *ret_dd = &fd->returns;
    bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
    DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : nullptr;
    if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

    node_t ret_type = cir_type_list(c2m, ret_dd, pos);
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

	    node_t spec_decl = cir_param_decl(c2m, ptype, pname, pos);
	    c2mir_op_append(c2m, param_list, spec_decl);
	}
    }
    c2mir_set_node_pos(c2m, param_list, pos);

    // Function declarator
    node_t func_inner = c2mir_new_node1(c2m, N_FUNC, param_list);
    c2mir_set_node_pos(c2m, func_inner, pos);

    // DECL(ID("name"), LIST(FUNC(...) [, POINTER]))
    // c2mir puts POINTER AFTER FUNC for pointer return types.
    node_t func_id = c2mir_new_str_node(c2m, N_ID, tf->var.name.c_str(),
					tf->var.name.size() + 1, pos);
    node_t decl_list = c2mir_new_node(c2m, N_LIST);
    c2mir_op_append(c2m, decl_list, func_inner);
    if (ret_is_ptr) {
	node_t pointer = c2mir_new_node1(c2m, N_POINTER, c2mir_new_node(c2m, N_LIST));
	c2mir_set_node_pos(c2m, pointer, pos);
	c2mir_op_append(c2m, decl_list, pointer);
    }
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

    // Return type — unwrap pointer if needed
    DataDef *ret_dd = &fd->returns;
    bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
    DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : nullptr;
    if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

    node_t ret_type = cir_type_list(c2m, ret_dd, pos);

    // Parameters
    node_t param_list = c2mir_new_node(c2m, N_LIST);

    if (fd->parameters.empty()) {
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
	for (size_t i = 0; i < fd->parameters.size(); i++) {
	    DataDef *ptype = fd->parameters[i];
	    const char *pname = "p";
	    if (tf->method && i < tf->method->parameters.size())
		pname = tf->method->parameters[i]->name.c_str();

	    node_t spec_decl = cir_param_decl(c2m, ptype, pname, pos);
	    c2mir_op_append(c2m, param_list, spec_decl);
	}
    }
    c2mir_set_node_pos(c2m, param_list, pos);

    // Function declarator: FUNC(param_list)
    node_t func_inner = c2mir_new_node1(c2m, N_FUNC, param_list);
    c2mir_set_node_pos(c2m, func_inner, pos);

    // Declarator: DECL(ID("name"), LIST(FUNC(...) [, POINTER]))
    node_t func_id = c2mir_new_str_node(c2m, N_ID, tf->var.name.c_str(),
					tf->var.name.size() + 1, pos);
    node_t decl_list = c2mir_new_node(c2m, N_LIST);
    c2mir_op_append(c2m, decl_list, func_inner);
    if (ret_is_ptr) {
	node_t pointer = c2mir_new_node1(c2m, N_POINTER, c2mir_new_node(c2m, N_LIST));
	c2mir_set_node_pos(c2m, pointer, pos);
	c2mir_op_append(c2m, decl_list, pointer);
    }
    c2mir_set_node_pos(c2m, decl_list, pos);
    node_t decl = c2mir_new_node2(c2m, N_DECL, func_id, decl_list);
    c2mir_set_node_pos(c2m, decl, pos);

    // Body: translate compound statement
    node_t body = cir_translate_block(c2m, (TokenCpnd *)tf);

    // FUNC_DEF(ret_type, decl, kr_decl_list, body)
    node_t func_def = c2mir_new_node4(c2m, N_FUNC_DEF,
	ret_type, decl,
	c2mir_new_node(c2m, N_LIST),
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

    // Pass 0: Emit struct definitions.
    // Only emit structs that have members (skip forward declarations and
    // builtins that might not be fully initialized).
    std::set<std::string> emitted_structs;
    for (auto &kv : prog->struct_map) {
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(kv.second);
	if (!sdd || sdd->members.empty()) continue;
	if (emitted_structs.count(sdd->name)) continue;
	emitted_structs.insert(sdd->name);
	c2mir_pos_t spos = { "<struct>", 1, 0 };
	node_t sd = cir_struct_def(c2m, sdd, spos);
	if (sd) c2mir_op_append(c2m, top_list, sd);
    }

    // Pass 0.5: Emit global variable declarations.
    // Skip function-typed variables (they're emitted as FUNC_DEF in Pass 2).
    if (prog->tkProgram) {
	for (auto *v : prog->tkProgram->variables) {
	    if (!v) continue;
	    if (dynamic_cast<FuncDef *>(v->type)) continue;
	    c2mir_pos_t gpos = { "<global>", 1, 0 };
	    node_t gd = cir_var_decl(c2m, v, gpos);
	    if (gd) c2mir_op_append(c2m, top_list, gd);
	}
    }

    // Pass 0.75: Emit extern function prototypes from funcdef_map.
    // These are functions declared via `extern` or built-in headers that
    // aren't in pending_funcs (user-defined). c2mir needs to see them
    // for correct type-checking of calls.
    std::set<std::string> user_func_names;
    for (TokenFunc *tf : funcs)
	user_func_names.insert(tf->var.name);

    for (auto &kv : prog->funcdef_map) {
	const std::string &fname = kv.first;
	FuncDef *fd = kv.second;
	if (!fd || user_func_names.count(fname)) continue;

	c2mir_pos_t epos = { "<extern>", 1, 0 };

	// Return type — unwrap pointer if needed
	DataDef *ret_dd = &fd->returns;
	bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
	DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : nullptr;
	if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

	node_t ret_type = cir_type_list(c2m, ret_dd, epos);
	// Add EXTERN to the type specifier list
	node_t ext_list = c2mir_new_node(c2m, N_LIST);
	c2mir_op_append(c2m, ext_list, c2mir_new_node(c2m, N_EXTERN));
	// Copy the type spec from ret_type's children
	node_t spec = cir_type_spec(c2m, ret_dd);
	if (ret_dd && ret_dd->is_struct()) {
	    DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(ret_dd);
	    if (sdd) {
		node_t sid = c2mir_new_str_node(c2m, N_ID, sdd->name.c_str(),
						sdd->name.size() + 1, epos);
		spec = c2mir_new_node2(c2m, N_STRUCT, sid, c2mir_new_node(c2m, N_IGNORE));
	    }
	}
	c2mir_op_append(c2m, ext_list, spec);
	c2mir_set_node_pos(c2m, ext_list, epos);
	node_t share = c2mir_new_node1(c2m, N_SHARE, ext_list);

	// Parameters
	node_t param_list = c2mir_new_node(c2m, N_LIST);
	if (fd->parameters.empty() && fd->is_void_params) {
	    node_t void_spec = c2mir_new_node1(c2m, N_LIST, c2mir_new_node(c2m, N_VOID));
	    node_t void_decl = c2mir_new_node2(c2m, N_DECL,
		c2mir_new_node(c2m, N_IGNORE), c2mir_new_node(c2m, N_LIST));
	    node_t void_param = c2mir_new_node2(c2m, N_TYPE, void_spec, void_decl);
	    c2mir_op_append(c2m, param_list, void_param);
	} else {
	    // For variadic functions, the parser adds a dummy int64 param for "...".
	    // Skip that last dummy parameter — we emit DOTS instead.
	    size_t nparam = fd->parameters.size();
	    if (fd->is_varargs && nparam > 0) nparam--;
	    for (size_t i = 0; i < nparam; i++) {
		char pname[16];
		snprintf(pname, sizeof(pname), "p%zu", i);
		node_t pd = cir_param_decl(c2m, fd->parameters[i], pname, epos);
		c2mir_op_append(c2m, param_list, pd);
	    }
	}
	// Variadic: append N_DOTS
	if (fd->is_varargs) {
	    node_t dots = c2mir_new_node(c2m, N_DOTS);
	    c2mir_set_node_pos(c2m, dots, epos);
	    c2mir_op_append(c2m, param_list, dots);
	}
	c2mir_set_node_pos(c2m, param_list, epos);

	node_t func_inner = c2mir_new_node1(c2m, N_FUNC, param_list);
	c2mir_set_node_pos(c2m, func_inner, epos);

	node_t func_id = c2mir_new_str_node(c2m, N_ID, fname.c_str(),
					     fname.size() + 1, epos);
	node_t decl_list = c2mir_new_node(c2m, N_LIST);
	c2mir_op_append(c2m, decl_list, func_inner);
	if (ret_is_ptr) {
	    node_t pointer = c2mir_new_node1(c2m, N_POINTER, c2mir_new_node(c2m, N_LIST));
	    c2mir_set_node_pos(c2m, pointer, epos);
	    c2mir_op_append(c2m, decl_list, pointer);
	}
	c2mir_set_node_pos(c2m, decl_list, epos);
	node_t decl = c2mir_new_node2(c2m, N_DECL, func_id, decl_list);
	c2mir_set_node_pos(c2m, decl, epos);

	node_t proto = c2mir_new_node(c2m, N_SPEC_DECL);
	c2mir_op_append(c2m, proto, share);
	c2mir_op_append(c2m, proto, decl);
	c2mir_op_append(c2m, proto, c2mir_new_node(c2m, N_IGNORE));
	c2mir_op_append(c2m, proto, c2mir_new_node(c2m, N_IGNORE));
	c2mir_op_append(c2m, proto, c2mir_new_node(c2m, N_IGNORE));
	c2mir_set_node_pos(c2m, proto, epos);

	c2mir_op_append(c2m, top_list, proto);
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

// -----------------------------------------------------------------------
// Import resolver for MIR linking — finds C library symbols via dlsym
// -----------------------------------------------------------------------

static void *cir_import_resolver(const char *name)
{
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr)
	DBG(std::cerr << "cir_import_resolver: unresolved: " << name << std::endl);
    return addr;
}

// -----------------------------------------------------------------------
// Full CIR pipeline: parse → translate → compile → JIT execute
// -----------------------------------------------------------------------

int madc_cir_execute(Program *prog, const char *source_name,
		     int user_argc, char **user_argv,
		     bool dump_tree)
{
    MIR_context_t ctx = MIR_init();
    c2mir_init(ctx);
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, 1);

    c2m_ctx_t c2m = cir_init(ctx);
    if (!c2m) {
	fprintf(stderr, "madc_cir_execute: cir_init failed\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    node_t tree = cir_translate(c2m, prog);
    if (!tree) {
	fprintf(stderr, "madc_cir_execute: cir_translate failed\n");
	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    if (dump_tree) {
	fprintf(stderr, "=== CIR TREE (pre-check) ===\n");
	c2mir_dump_tree(c2m, stderr, tree);
	fprintf(stderr, "=== END CIR TREE ===\n");
    }

    int ok = cir_compile(ctx, c2m, tree, source_name);
    if (!ok) {
	fprintf(stderr, "madc_cir_execute: cir_compile failed\n");
	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
    if (!mod) {
	fprintf(stderr, "madc_cir_execute: no module produced\n");
	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    MIR_load_module(ctx, mod);
    MIR_link(ctx, MIR_set_gen_interface, cir_import_resolver);

    void *code = nullptr;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item &&
	    strcmp(item->u.func->name, "main") == 0) {
	    code = MIR_gen(ctx, item);
	    break;
	}
    }

    if (!code) {
	fprintf(stderr, "madc_cir_execute: main() not found\n");
	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    int result = ((int (*)(int, char **))code)(user_argc, user_argv);

    cir_finish(c2m);
    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);

    return result;
}
