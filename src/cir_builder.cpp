/* cir_builder.cpp — CirBuilder implementation.
 *
 * Builds cir_node AST trees that are simultaneously:
 *   1. Valid c2mir node_t trees (passable to c2mir_compile_tree)
 *   2. madc AST trees with origin pointers, typedef names, etc.
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
#include "cir_builder.h"

extern "C" {
#include "c2mir/c2mir_api.h"
}

extern thread_local bool madc_verbose;

// -----------------------------------------------------------------------
// CirBuilder core
// -----------------------------------------------------------------------

CirBuilder::CirBuilder(c2m_ctx_t c2m_ctx) : c2m(c2m_ctx), m_prog(NULL) {}
CirBuilder::~CirBuilder() {}

cir_node *CirBuilder::make(c2mir_node_code_t code, TokenBase *origin)
{
	cir_node *cn = arena.alloc();
	cn->base.code = (node_code_t)code;
	cn->base.uid = c2mir_next_uid(c2m);
	cn->base.attr = NULL;
	c2mir_init_node_ops((node_t)cn);

	cn->origin = origin;
	cn->datadef = NULL;
	cn->typedef_name = NULL;
	cn->src_lang = cslC;  // default; caller can override

	if (origin) {
		cn->src_file = origin->file;
		cn->src_line = origin->line;
		cn->src_column = origin->column;
		set_pos(cn, origin);
	}

	return cn;
}

void CirBuilder::set_pos(cir_node *cn, const char *file, int line, int col)
{
	c2mir_pos_t pos = { file, line, col };
	c2mir_set_node_pos(c2m, cn->as_node(), pos);
}

void CirBuilder::set_pos(cir_node *cn, TokenBase *tb)
{
	if (tb)
		set_pos(cn, tb->file, tb->line, tb->column);
}

// -----------------------------------------------------------------------
// Leaf node builders
// -----------------------------------------------------------------------

node_t CirBuilder::id(const char *name, TokenBase *origin)
{
	cir_node *cn = make(N_ID, origin);
	const char *interned = c2mir_uniq_str(c2m, name, strlen(name) + 1);
	cn->base.u.s.s = interned;
	cn->base.u.s.len = strlen(name) + 1;
	return cn->as_node();
}

node_t CirBuilder::integer(long val, TokenBase *origin)
{
	cir_node *cn = make(N_I, origin);
	cn->base.u.l = val;
	return cn->as_node();
}

node_t CirBuilder::real(double val, TokenBase *origin)
{
	cir_node *cn = make(N_D, origin);
	cn->base.u.d = val;
	return cn->as_node();
}

node_t CirBuilder::str(const char *s, size_t len, TokenBase *origin)
{
	cir_node *cn = make(N_STR, origin);
	const char *interned = c2mir_uniq_str(c2m, s, len);
	cn->base.u.s.s = interned;
	cn->base.u.s.len = len;
	return cn->as_node();
}

node_t CirBuilder::ignore()
{
	cir_node *cn = make(N_IGNORE, NULL);
	return cn->as_node();
}

// -----------------------------------------------------------------------
// Composite node builders
// -----------------------------------------------------------------------

node_t CirBuilder::list()
{
	cir_node *cn = make(N_LIST, NULL);
	return cn->as_node();
}

node_t CirBuilder::simple(c2mir_node_code_t code, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	return cn->as_node();
}

node_t CirBuilder::append(node_t parent, node_t child)
{
	return c2mir_op_append(c2m, parent, child);
}

node_t CirBuilder::node1(c2mir_node_code_t code, node_t op1, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	return n;
}

node_t CirBuilder::node2(c2mir_node_code_t code, node_t op1, node_t op2, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	return n;
}

node_t CirBuilder::node3(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	c2mir_op_append(c2m, n, op3);
	return n;
}

node_t CirBuilder::node4(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, node_t op4, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	c2mir_op_append(c2m, n, op3);
	c2mir_op_append(c2m, n, op4);
	return n;
}

node_t CirBuilder::node5(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, node_t op4, node_t op5, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	c2mir_op_append(c2m, n, op3);
	c2mir_op_append(c2m, n, op4);
	c2mir_op_append(c2m, n, op5);
	return n;
}

// -----------------------------------------------------------------------
// Type builders
// -----------------------------------------------------------------------

node_t CirBuilder::pointer()
{
	return node1(N_POINTER, list());
}

// Append type specifier nodes for a DataDef into a LIST node.
void CirBuilder::append_type_specs(node_t lst, DataDef *dd)
{
	if (!dd) { append(lst, simple(N_INT)); return; }

	DataType dt = dd->rawtype();
	switch (dt) {
	case DataType::dtVOID:   append(lst, simple(N_VOID)); break;
	case DataType::dtCHAR:   append(lst, simple(N_CHAR)); break;
	case DataType::dtINT16:  append(lst, simple(N_SHORT)); break;
	case DataType::dtINT32:  append(lst, simple(N_INT)); break;
	case DataType::dtINT64:  append(lst, simple(N_LONG)); break;
	case DataType::dtUINT8:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_CHAR));
		break;
	case DataType::dtUINT16:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_SHORT));
		break;
	case DataType::dtUINT32:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_INT));
		break;
	case DataType::dtUINT64:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_LONG));
		break;
	case DataType::dtFLOAT:  append(lst, simple(N_FLOAT)); break;
	case DataType::dtDOUBLE: append(lst, simple(N_DOUBLE)); break;
	default:
		append(lst, simple(N_INT));
	}
}

// Build a type specifier LIST. If typedef_alias is set, emit ID("alias")
// instead of raw type nodes — c2mir's checker resolves the typedef.
node_t CirBuilder::type_list(DataDef *dd, const std::string &typedef_alias)
{
	node_t lst = list();

	// If a typedef name is available, emit ID("alias") — matches c2m's behavior
	if (!typedef_alias.empty()) {
		append(lst, id(typedef_alias.c_str()));
		return lst;
	}

	// Struct types: LIST(STRUCT(ID("name"), IGNORE))
	if (dd && dd->is_struct()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
		if (sdd) {
			node_t sref = node2(N_STRUCT, id(sdd->name.c_str()), ignore());
			append(lst, sref);
			return lst;
		}
	}

	append_type_specs(lst, dd);
	return lst;
}

// -----------------------------------------------------------------------
// Declaration builders
// -----------------------------------------------------------------------

node_t CirBuilder::param_decl(DataDef *ptype, const char *pname)
{
	DataDef *base_dd = ptype;
	bool is_ptr = base_dd && base_dd->is_pointer();
	DataDefPTR *ptr_dd = is_ptr ? dynamic_cast<DataDefPTR *>(base_dd) : NULL;
	if (ptr_dd && ptr_dd->base_type)
		base_dd = ptr_dd->base_type;

	node_t pspec = type_list(base_dd);
	node_t pid = id(pname);
	node_t pdecl_list = list();
	if (is_ptr)
		append(pdecl_list, pointer());
	node_t pdecl = node2(N_DECL, pid, pdecl_list);

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, pspec);
	append(spec_decl, pdecl);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	return spec_decl;
}

node_t CirBuilder::var_decl(Variable *v, TokenBase *origin)
{
	DataDef *base_dd = v->type;
	bool is_ptr = base_dd && base_dd->is_pointer();
	DataDefPTR *ptr_dd = is_ptr ? dynamic_cast<DataDefPTR *>(base_dd) : NULL;
	if (ptr_dd && ptr_dd->base_type)
		base_dd = ptr_dd->base_type;

	// Emit ID("alias") for a variable declared via a typedef (matches c2m).
	// Pointer usages keep the alias as the type spec and carry the explicit
	// stars on the declarator below.
	node_t tl = !v->typedef_name.empty()
			? type_list(v->type, v->typedef_name)
			: type_list(base_dd);

	// Storage class qualifiers
	if (v->flags & vfSTATIC) {
		node_t new_list = list();
		append(new_list, simple(N_STATIC));
		if (base_dd && base_dd->is_struct()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
			if (sdd)
				append(new_list, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
			else
				append_type_specs(new_list, base_dd);
		} else {
			append_type_specs(new_list, base_dd);
		}
		tl = new_list;
	}
	if (v->flags & vfEXTERN) {
		node_t new_list = list();
		append(new_list, simple(N_EXTERN));
		append_type_specs(new_list, base_dd);
		tl = new_list;
	}

	node_t share = node1(N_SHARE, tl);
	node_t var_id = id(v->name.c_str(), origin);
	node_t decl_list = list();

	int decl_stars = explicit_star_count(v->type, v->typedef_name);
	if (decl_stars >= 0) {
		for (int s = 0; s < decl_stars; s++)
			append(decl_list, pointer());
	} else if (is_ptr) {
		append(decl_list, pointer());
	}

	if (v->is_fixed_array() && !v->dims.empty()) {
		node_t size = integer(v->dims[0]);
		node_t arr = node3(N_ARR, ignore(), list(), size);
		append(decl_list, arr);
	}

	node_t var_decl_node = node2(N_DECL, var_id, decl_list);
	node_t init_node = ignore();
	// TODO: initializers will be ported later when we integrate fully

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, share);
	append(spec_decl, var_decl_node);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, init_node);
	return spec_decl;
}

// Count pointer-indirection levels of a DataDef (int** -> 2, NODE -> 0).
static int dd_ptr_depth(DataDef *dd)
{
	int depth = 0;
	while (dd && dd->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
		if (!p) break;
		dd = p->base_type;
		depth++;
	}
	return depth;
}

int CirBuilder::explicit_star_count(DataDef *full_type, const std::string &alias)
{
	if (alias.empty())
		return -1;
	int full_depth = dd_ptr_depth(full_type);
	int base_depth = 0;
	if (m_prog) {
		// Resolve the alias to the typedef's own DataDef so its base pointer
		// depth can be subtracted from the usage-site total.
		datatype_map_iter it = m_prog->datatype_map.find(alias);
		if (it != m_prog->datatype_map.end() && it->second)
			base_depth = dd_ptr_depth(&it->second->definition);
	}
	int stars = full_depth - base_depth;
	return stars < 0 ? 0 : stars;
}

node_t CirBuilder::member_node(const memberpair_t &m)
{
	DataDef *mtype = m.second;            // full member type, stars included
	const std::string &mtypedef = m.typedef_name;

	// Type specifier. A member declared via a typedef emits ID("alias")
	// regardless of pointer-ness — the stars belong on the declarator, not
	// the type spec (matches c2m). The star count is the explicit indirection
	// written at the usage site, i.e. the type's total depth minus the
	// typedef's own base depth (handles `typedef T *TP; TP x;` correctly).
	int stars = explicit_star_count(mtype, mtypedef);
	node_t mspec;
	if (!mtypedef.empty()) {
		mspec = type_list(mtype, mtypedef);
	} else {
		DataDef *mbase = mtype;
		if (mbase && mbase->is_pointer()) {
			DataDefPTR *mptr = dynamic_cast<DataDefPTR *>(mbase);
			if (mptr && mptr->base_type) mbase = mptr->base_type;
		}
		mspec = type_list(mbase);
	}

	node_t mshare = node1(N_SHARE, mspec);
	node_t mid = id(m.first.c_str(), m.origin);
	node_t mdecl_list = list();
	if (!mtypedef.empty()) {
		for (int s = 0; s < stars; s++)
			append(mdecl_list, pointer());
	} else if (mtype && mtype->is_pointer()) {
		append(mdecl_list, pointer());
	}
	node_t mdecl = node2(N_DECL, mid, mdecl_list);

	node_t member = simple(N_MEMBER, m.origin);
	append(member, mshare);
	append(member, mdecl);
	append(member, ignore());
	append(member, ignore());
	return member;
}

node_t CirBuilder::struct_def(DataDefSTRUCT *sdd)
{
	node_t struct_id = id(sdd->name.c_str());
	node_t member_list = list();

	for (size_t i = 0; i < sdd->members.size(); i++) {
		append(member_list, member_node(sdd->members[i]));
	}

	node_t struct_node = node2(N_STRUCT, struct_id, member_list);
	node_t tl = node1(N_LIST, struct_node);

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, tl);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	return spec_decl;
}

node_t CirBuilder::typedef_decl(const std::string &alias, DataDef *dd,
				const std::set<std::string> &emitted_structs,
				bool force_incomplete_struct)
{
	if (!dd) return NULL;

	node_t tl = list();
	append(tl, simple(N_TYPEDEF));

	// Unwrap pointer for base type
	DataDef *base_dd = dd;
	bool is_ptr = dd->is_pointer();
	DataDefPTR *ptr_dd = is_ptr ? dynamic_cast<DataDefPTR *>(dd) : NULL;
	if (ptr_dd && ptr_dd->base_type)
		base_dd = ptr_dd->base_type;

	// Type specifier
	if (base_dd->is_struct()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
		if (sdd) {
			if (force_incomplete_struct || emitted_structs.count(sdd->name) || sdd->members.empty()) {
				// Forward reference / already-emitted / incomplete:
				// STRUCT(tag, IGNORE). The full body is emitted at the
				// struct's own definition point.
				append(tl, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
			} else {
				// Emit struct definition inline (typedef struct { ... } NAME)
				node_t struct_id_node = id(sdd->name.c_str());
				node_t ml = list();
				for (size_t i = 0; i < sdd->members.size(); i++) {
					append(ml, member_node(sdd->members[i]));
				}
				append(tl, node2(N_STRUCT, struct_id_node, ml));
			}
		}
	} else {
		append_type_specs(tl, base_dd);
	}

	node_t share = node1(N_SHARE, tl);

	// Declarator: DECL(ID("alias"), LIST([POINTER]))
	node_t alias_id = id(alias.c_str());
	node_t decl_list = list();
	if (is_ptr)
		append(decl_list, pointer());

	node_t decl = node2(N_DECL, alias_id, decl_list);

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, share);
	append(spec_decl, decl);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	return spec_decl;
}

node_t CirBuilder::func_proto(TokenFunc *tf)
{
	FuncDef *fd = dynamic_cast<FuncDef *>(tf->var.type);
	if (!fd) return NULL;

	DataDef *ret_dd = &fd->returns;
	bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
	DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
	if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

	node_t ret_type = type_list(ret_dd);
	node_t share = node1(N_SHARE, ret_type);

	// Parameters
	node_t param_list = list();
	if (fd->parameters.empty()) {
		node_t void_spec = node1(N_LIST, simple(N_VOID));
		node_t void_decl = node2(N_DECL, ignore(), list());
		node_t void_param = node2(N_TYPE, void_spec, void_decl);
		append(param_list, void_param);
	} else {
		for (size_t i = 0; i < fd->parameters.size(); i++) {
			DataDef *ptype = fd->parameters[i];
			const char *pname = "p";
			if (tf->method && i < tf->method->parameters.size())
				pname = tf->method->parameters[i]->name.c_str();
			append(param_list, param_decl(ptype, pname));
		}
	}

	node_t func_inner = node1(N_FUNC, param_list);

	node_t func_id = id(tf->var.name.c_str(), tf);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_is_ptr)
		append(decl_list, pointer());

	node_t decl = node2(N_DECL, func_id, decl_list);

	node_t proto = simple(N_SPEC_DECL);
	append(proto, share);
	append(proto, decl);
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());
	return proto;
}

// -----------------------------------------------------------------------
// Expression translation
// -----------------------------------------------------------------------

node_t CirBuilder::translate_expr(TokenBase *tb)
{
	if (!tb) return ignore();

	// Integer literal
	if (tb->type() == TokenType::ttInteger)
		return integer(tb->ival(), tb);

	// Real literal
	if (tb->type() == TokenType::ttReal)
		return real(tb->dval(), tb);

	// String literal
	if (tb->type() == TokenType::ttString) {
		TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
		if (ti)
			return str(ti->str.c_str(), ti->str.size() + 1, tb);
	}

	// Variable reference
	if (tb->type() == TokenType::ttVariable) {
		TokenVar *tv = dynamic_cast<TokenVar *>(tb);
		if (tv) {
			if (tv->var.is_constant() && tv->var.type &&
			    tv->var.type->is_integer() && !tv->var.type->is_pointer())
				return integer(tv->var.get<int64_t>(), tb);
			if (tv->var.name.compare(0, 11, "__literal__") == 0) {
				const std::string &content = tv->var.name.substr(11);
				return str(content.c_str(), content.size() + 1, tb);
			}
			return id(tv->var.name.c_str(), tb);
		}
	}

	// Identifier
	if (tb->type() == TokenType::ttIdentifier) {
		TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
		if (ti)
			return id(ti->str.c_str(), tb);
	}

	// Ternary
	{
		TokenTerQ *tq = dynamic_cast<TokenTerQ *>(tb);
		if (tq)
			return node3(N_COND,
				translate_expr(tq->condition),
				translate_expr(tq->true_expr),
				translate_expr(tq->false_expr), tb);
	}

	// Address-of variable
	{
		TokenAddrOf *ta = dynamic_cast<TokenAddrOf *>(tb);
		if (ta)
			return node1(N_ADDR, id(ta->var.name.c_str(), tb), tb);
	}

	// Address-of expression
	{
		TokenAddrExpr *tae = dynamic_cast<TokenAddrExpr *>(tb);
		if (tae)
			return node1(N_ADDR, translate_expr(tae->expr), tb);
	}

	// Dereference variable
	{
		TokenDeref *td = dynamic_cast<TokenDeref *>(tb);
		if (td)
			return node1(N_DEREF, id(td->var.name.c_str(), tb), tb);
	}

	// Dereference expression
	{
		TokenDerefExpr *tde = dynamic_cast<TokenDerefExpr *>(tb);
		if (tde)
			return node1(N_DEREF, translate_expr(tde->expr), tb);
	}

	// Array subscript
	{
		TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(tb);
		if (tsub)
			return node2(N_IND,
				id(tsub->object.name.c_str(), tb),
				translate_expr(tsub->index), tb);
	}

	// Struct member access
	{
		TokenMember *tm = dynamic_cast<TokenMember *>(tb);
		if (tm) {
			node_t obj;
			if (tm->parent_expr)
				obj = translate_expr(tm->parent_expr);
			else
				obj = id(tm->object.name.c_str(), tb);
			node_t member = id(tm->var.name.c_str(), tb);
			c2mir_node_code_t code = tm->object.type->is_pointer() ? N_DEREF_FIELD : N_FIELD;
			return node2(code, obj, member, tb);
		}
	}

	// sizeof
	{
		TokenTypeQuery *ttq = dynamic_cast<TokenTypeQuery *>(tb);
		if (ttq && ttq->query_type) {
			node_t tl = type_list(ttq->query_type);
			node_t type_decl = node2(N_DECL, ignore(), list());
			node_t type_node = node2(N_TYPE, tl, type_decl);
			c2mir_node_code_t code = ttq->want_alignof ? N_ALIGNOF : N_SIZEOF;
			return node1(code, type_node, tb);
		}
	}

	// Cast
	{
		TokenCast *tc = dynamic_cast<TokenCast *>(tb);
		if (tc) {
			DataDef *cast_dd = tc->cast_type;
			bool cast_is_ptr = cast_dd && cast_dd->is_pointer();
			DataDefPTR *cast_ptr = cast_is_ptr ? dynamic_cast<DataDefPTR *>(cast_dd) : NULL;
			if (cast_ptr && cast_ptr->base_type) cast_dd = cast_ptr->base_type;

			node_t tl = type_list(cast_dd);
			node_t cast_decl_list = list();
			if (cast_is_ptr)
				append(cast_decl_list, pointer());
			node_t type_decl = node2(N_DECL, ignore(), cast_decl_list);
			node_t type_node = node2(N_TYPE, tl, type_decl);
			return node2(N_CAST, type_node, translate_expr(tc->expr), tb);
		}
	}

	// Operators
	if (tb->is_operator()) {
		TokenOperator *top = dynamic_cast<TokenOperator *>(tb);

		// Inc/Dec
		if (tb->id() == TokenID::tkInc || tb->id() == TokenID::tkDec) {
			bool is_post = (top->left != NULL);
			TokenBase *operand_tb = is_post ? top->left : top->right;
			node_t operand = translate_expr(operand_tb);
			c2mir_node_code_t code;
			if (tb->id() == TokenID::tkInc)
				code = is_post ? N_POST_INC : N_INC;
			else
				code = is_post ? N_POST_DEC : N_DEC;
			return node1(code, operand, tb);
		}

		// Unary
		if (top && top->argc() == 1 && top->right) {
			node_t operand = translate_expr(top->right);
			if (tb->id() == TokenID::tkNeg)
				return node2(N_SUB, integer(0, tb), operand, tb);
			c2mir_node_code_t code;
			switch (tb->id()) {
			case TokenID::tkLnot: code = N_NOT; break;
			case TokenID::tkBnot: code = N_BITWISE_NOT; break;
			default:
				DBG(std::cerr << "cir: unhandled unary op " << (int)tb->id() << std::endl);
				code = N_NOT;
			}
			return node1(code, operand, tb);
		}

		// Binary
		if (top && top->left && top->right) {
			node_t left = translate_expr(top->left);
			node_t right = translate_expr(top->right);

			c2mir_node_code_t code;
			switch (tb->id()) {
			case TokenID::tkAdd:    code = N_ADD; break;
			case TokenID::tkSub:    code = N_SUB; break;
			case TokenID::tkMul:    code = N_MUL; break;
			case TokenID::tkDiv:    code = N_DIV; break;
			case TokenID::tkMod:    code = N_MOD; break;
			case TokenID::tkAssign: code = N_ASSIGN; break;
			case TokenID::tkEquals: code = N_EQ; break;
			case TokenID::tkNotEq:  code = N_NE; break;
			case TokenID::tkLT:     code = N_LT; break;
			case TokenID::tkLE:     code = N_LE; break;
			case TokenID::tkGT:     code = N_GT; break;
			case TokenID::tkGE:     code = N_GE; break;
			case TokenID::tkBand:   code = N_AND; break;
			case TokenID::tkBor:    code = N_OR; break;
			case TokenID::tkXor:    code = N_XOR; break;
			case TokenID::tkLand:   code = N_ANDAND; break;
			case TokenID::tkLor:    code = N_OROR; break;
			case TokenID::tkBSL:    code = N_LSH; break;
			case TokenID::tkBSR:    code = N_RSH; break;
			case TokenID::tkComma:  code = N_COMMA; break;
			case TokenID::tkAddEq:  code = N_ADD_ASSIGN; break;
			case TokenID::tkSubEq:  code = N_SUB_ASSIGN; break;
			case TokenID::tkMulEq:  code = N_MUL_ASSIGN; break;
			case TokenID::tkDivEq:  code = N_DIV_ASSIGN; break;
			case TokenID::tkModEq:  code = N_MOD_ASSIGN; break;
			case TokenID::tkBandEq: code = N_AND_ASSIGN; break;
			case TokenID::tkBorEq:  code = N_OR_ASSIGN; break;
			case TokenID::tkXorEq:  code = N_XOR_ASSIGN; break;
			case TokenID::tkBSLEq:  code = N_LSH_ASSIGN; break;
			case TokenID::tkBSREq:  code = N_RSH_ASSIGN; break;
			default:
				DBG(std::cerr << "cir: unhandled binary op " << (int)tb->id() << std::endl);
				code = N_ADD;
			}
			return node2(code, left, right, tb);
		}
	}

	// Function call
	if (tb->type() == TokenType::ttCallFunc) {
		TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(tb);
		if (tcf) {
			referenced_funcs.insert(tcf->var.name);
			node_t func_id = id(tcf->var.name.c_str(), tb);
			node_t args = list();
			for (size_t i = 0; i < tcf->parameters.size(); i++)
				append(args, translate_expr(tcf->parameters[i]));
			return node2(N_CALL, func_id, args, tb);
		}
	}

	DBG(std::cerr << "cir: unhandled expr type=" << (int)tb->type()
		      << " id=" << (int)tb->id() << std::endl);
	return integer(0, tb);
}

// -----------------------------------------------------------------------
// Statement translation
// -----------------------------------------------------------------------

node_t CirBuilder::translate_return(TokenRETURN *tr)
{
	node_t expr = tr->returns ? translate_expr(tr->returns) : ignore();
	return node2(N_RETURN, list(), expr, tr);
}

node_t CirBuilder::translate_if(TokenIF *ti)
{
	node_t cond = translate_expr(ti->condition);
	node_t then_body = translate_stmt(ti->statement);
	node_t else_body = ti->elsestmt ? translate_stmt(ti->elsestmt) : ignore();
	return node4(N_IF, list(), cond, then_body, else_body, ti);
}

node_t CirBuilder::translate_while(TokenBase *tw)
{
	TokenWHILE *w = dynamic_cast<TokenWHILE *>(tw);
	if (!w) return ignore();
	return node3(N_WHILE, list(), translate_expr(w->condition),
		     translate_stmt(w->statement), tw);
}

node_t CirBuilder::translate_for(TokenFOR *tf)
{
	node_t init = tf->initialize ? translate_expr(tf->initialize) : ignore();
	node_t cond = tf->condition ? translate_expr(tf->condition) : ignore();
	node_t incr = tf->increment ? translate_expr(tf->increment) : ignore();
	node_t body = translate_stmt(tf->statement);
	return node5(N_FOR, list(), init, cond, incr, body, tf);
}

node_t CirBuilder::translate_do(TokenDO *td)
{
	return node3(N_DO, list(), translate_expr(td->condition),
		     translate_stmt(td->statement), td);
}

node_t CirBuilder::translate_switch(TokenSWITCH *ts)
{
	node_t expr = translate_expr(ts->expression);
	node_t block_items = list();

	for (size_t ci = 0; ci < ts->cases.size(); ci++) {
		TokenCASE *tc = ts->cases[ci];
		bool is_default = (tc->value == NULL);

		node_t label;
		if (is_default) {
			label = simple(N_DEFAULT);
		} else {
			label = node1(N_CASE, translate_expr(tc->value), tc);
		}

		for (size_t si = 0; si < tc->statements.size(); si++) {
			node_t s = translate_stmt(tc->statements[si]);
			if (!s) continue;
			if (si == 0) {
				node_t label_list = node1(N_LIST, label);
				if (tc->statements.empty()) {
					append(block_items, node2(N_EXPR, label_list, integer(0)));
				} else {
					TokenRETURN *ret = dynamic_cast<TokenRETURN *>(tc->statements[si]);
					if (ret) {
						node_t retexpr = ret->returns ? translate_expr(ret->returns) : ignore();
						append(block_items, node2(N_RETURN, label_list, retexpr));
					} else if (dynamic_cast<TokenBREAK *>(tc->statements[si])) {
						append(block_items, node1(N_BREAK, label_list));
					} else {
						node_t inner = translate_expr(tc->statements[si]);
						append(block_items, node2(N_EXPR, label_list, inner));
					}
				}
			} else {
				append(block_items, s);
			}
		}

		if (tc->statements.empty())
			append(block_items, node2(N_EXPR, node1(N_LIST, label), integer(0)));
	}

	// Default case
	if (ts->defaultcase && ts->default_index < 0) {
		TokenCASE *dc = ts->defaultcase;
		node_t def_label = simple(N_DEFAULT);
		for (size_t si = 0; si < dc->statements.size(); si++) {
			node_t s = translate_stmt(dc->statements[si]);
			if (!s) continue;
			if (si == 0) {
				node_t label_list = node1(N_LIST, def_label);
				TokenRETURN *ret = dynamic_cast<TokenRETURN *>(dc->statements[si]);
				if (ret) {
					node_t retexpr = ret->returns ? translate_expr(ret->returns) : ignore();
					append(block_items, node2(N_RETURN, label_list, retexpr));
				} else {
					node_t inner = translate_expr(dc->statements[si]);
					append(block_items, node2(N_EXPR, label_list, inner));
				}
			} else {
				append(block_items, s);
			}
		}
	}

	node_t body = node2(N_BLOCK, list(), block_items);
	return node3(N_SWITCH, list(), expr, body, ts);
}

node_t CirBuilder::translate_stmt(TokenBase *tb)
{
	if (!tb) return NULL;

	TokenRETURN *tr = dynamic_cast<TokenRETURN *>(tb);
	if (tr) return translate_return(tr);

	TokenIF *ti = dynamic_cast<TokenIF *>(tb);
	if (ti) return translate_if(ti);

	if (tb->id() == TokenID::tkWHILE)
		return translate_while(tb);

	TokenFOR *tf = dynamic_cast<TokenFOR *>(tb);
	if (tf) return translate_for(tf);

	{ TokenDO *td = dynamic_cast<TokenDO *>(tb);
	  if (td) return translate_do(td); }

	{ TokenSWITCH *ts = dynamic_cast<TokenSWITCH *>(tb);
	  if (ts) return translate_switch(ts); }

	// Goto
	{ TokenGOTO *tg = dynamic_cast<TokenGOTO *>(tb);
	  if (tg) return node2(N_GOTO, list(), id(tg->target.c_str(), tb), tb); }

	// Label (handled in translate_block)
	{ TokenLabel *tl = dynamic_cast<TokenLabel *>(tb);
	  if (tl) return NULL; }

	// Declaration
	{ TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)tb);
	  if (td) return var_decl(&td->var, td); }

	// Break
	if (tb->id() == TokenID::tkBREAK)
		return node1(N_BREAK, list(), tb);

	// Continue
	if (tb->id() == TokenID::tkCONT)
		return node1(N_CONTINUE, list(), tb);

	// Compound statement (block)
	TokenCpnd *tc = dynamic_cast<TokenCpnd *>(tb);
	if (tc) return translate_block(tc);

	// Expression statement
	return node2(N_EXPR, list(), translate_expr(tb), tb);
}

node_t CirBuilder::translate_block(TokenCpnd *tc)
{
	node_t empty_list = list();
	node_t items = list();

	TokenFunc *parent_func = dynamic_cast<TokenFunc *>(tc);
	size_t skip = 0;
	if (parent_func) {
		FuncDef *fd = dynamic_cast<FuncDef *>(parent_func->var.type);
		if (fd) skip = fd->parameters.size();
	}

	std::set<std::string> decl_vars;
	for (auto *ts : tc->statements) {
		TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)ts);
		if (td)
			decl_vars.insert(td->var.name);
	}

	for (size_t vi = skip; vi < tc->variables.size(); vi++) {
		Variable *v = tc->variables[vi];
		if (decl_vars.count(v->name)) continue;
		append(items, var_decl(v));
	}

	// Statements with label handling
	std::vector<std::string> pending_labels;
	for (size_t si = 0; si < tc->statements.size(); si++) {
		TokenBase *stb = (TokenBase *)tc->statements[si];
		TokenLabel *tl = dynamic_cast<TokenLabel *>(stb);
		if (tl) {
			pending_labels.push_back(tl->name);
			continue;
		}

		node_t s = translate_stmt(stb);
		if (!s) continue;

		if (!pending_labels.empty()) {
			node_t label_list = list();
			for (auto &lname : pending_labels) {
				node_t lid = id(lname.c_str());
				append(label_list, node1(N_LABEL, lid));
			}
			pending_labels.clear();
			append(items, node2(N_EXPR, label_list, integer(0)));
		}
		append(items, s);
	}

	if (!pending_labels.empty()) {
		node_t label_list = list();
		for (auto &lname : pending_labels) {
			node_t lid = id(lname.c_str());
			append(label_list, node1(N_LABEL, lid));
		}
		append(items, node2(N_EXPR, label_list, integer(0)));
	}

	return node2(N_BLOCK, empty_list, items, tc);
}

// -----------------------------------------------------------------------
// Function definition
// -----------------------------------------------------------------------

node_t CirBuilder::func_def(TokenFunc *tf)
{
	FuncDef *fd = dynamic_cast<FuncDef *>(tf->var.type);
	if (!fd) return NULL;

	DataDef *ret_dd = &fd->returns;
	bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
	DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
	if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

	node_t ret_type = type_list(ret_dd);

	// Parameters
	node_t param_list = list();
	if (fd->parameters.empty()) {
		node_t void_spec = node1(N_LIST, simple(N_VOID));
		node_t void_decl = node2(N_DECL, ignore(), list());
		append(param_list, node2(N_TYPE, void_spec, void_decl));
	} else {
		for (size_t i = 0; i < fd->parameters.size(); i++) {
			const char *pname = "p";
			if (tf->method && i < tf->method->parameters.size())
				pname = tf->method->parameters[i]->name.c_str();
			append(param_list, param_decl(fd->parameters[i], pname));
		}
	}

	node_t func_inner = node1(N_FUNC, param_list);

	node_t func_id = id(tf->var.name.c_str(), tf);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_is_ptr)
		append(decl_list, pointer());

	node_t decl = node2(N_DECL, func_id, decl_list);
	node_t body = translate_block((TokenCpnd *)tf);

	return node4(N_FUNC_DEF, ret_type, decl, list(), body, tf);
}

// -----------------------------------------------------------------------
// Top-level module translation
// -----------------------------------------------------------------------

node_t CirBuilder::translate_module(Program *prog)
{
	if (!prog) return NULL;
	m_prog = prog;

	node_t module = simple(N_MODULE);
	node_t top_list = list();

	// Collect all TokenFunc entries
	std::vector<TokenFunc *> funcs;
	for (auto it = prog->pending_funcs.begin();
	     it != prog->pending_funcs.end(); ++it) {
		TokenFunc *tf = dynamic_cast<TokenFunc *>(*it);
		if (tf && !tf->is_overridden)
			funcs.push_back(tf);
	}

	// Helper: resolve the (possibly pointer-wrapped) struct behind a DataDef.
	auto struct_behind = [](DataDef *dd) -> DataDefSTRUCT * {
		DataDefSTRUCT *s = dynamic_cast<DataDefSTRUCT *>(dd);
		if (!s) {
			DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
			if (p) s = dynamic_cast<DataDefSTRUCT *>(p->base_type);
		}
		return s;
	};

	// Pre-scan: structs that have a bare standalone definition entry
	// (`struct X {...};`). A typedef that references such a struct is a
	// forward reference and must emit STRUCT(tag, IGNORE) — the full body is
	// emitted once at the bare dkStruct, matching c2m's source structure.
	std::set<std::string> struct_def_points;
	for (auto &td : prog->top_decls) {
		if (td.kind == Program::DeclKind::dkStruct ||
		    td.kind == Program::DeclKind::dkUnion) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
			if (sdd && !sdd->members.empty())
				struct_def_points.insert(sdd->name);
		}
	}

	// Pass 0: top-level declarations in source order (faithful mirror).
	// Stamp each declaration node with the source position the parser
	// recorded (file/line); the token itself isn't in scope at the record
	// site, so position is threaded here. Member nodes carry the real
	// origin token (see member_node).
	auto stamp = [&](node_t n, Program::TopDecl &td) {
		if (!n || !td.file) return;
		cir_node *cn = CIR_NODE(n);
		cn->src_file = td.file;
		cn->src_line = td.line;
		set_pos(cn, td.file, td.line, 0);
	};
	std::set<std::string> emitted_structs;
	for (auto &td : prog->top_decls) {
		switch (td.kind) {
		case Program::DeclKind::dkTypedef: {
			DataDefSTRUCT *sdd = struct_behind(td.dd);
			// Forward ref iff the struct is defined by a separate bare
			// dkStruct and hasn't been emitted yet -> STRUCT(tag, IGNORE).
			bool forward = sdd && struct_def_points.count(sdd->name)
					   && !emitted_structs.count(sdd->name);
			node_t n = typedef_decl(td.name, td.dd, emitted_structs, forward);
			if (n) { stamp(n, td); append(top_list, n); }
			// A combined `typedef struct X {...} Y;` (no separate bare
			// def) emits the body inline here -> mark it emitted.
			if (sdd && !sdd->members.empty() && !struct_def_points.count(sdd->name))
				emitted_structs.insert(sdd->name);
			break;
		}
		case Program::DeclKind::dkStruct:
		case Program::DeclKind::dkUnion: {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
			if (sdd && !sdd->members.empty() && !emitted_structs.count(sdd->name)) {
				emitted_structs.insert(sdd->name);
				node_t sd = struct_def(sdd);
				if (sd) { stamp(sd, td); append(top_list, sd); }
			}
			break;
		}
		case Program::DeclKind::dkGlobalVar: {
			if (td.var && !dynamic_cast<FuncDef *>(td.var->type)) {
				node_t gd = var_decl(td.var);
				if (gd) { stamp(gd, td); append(top_list, gd); }
			}
			break;
		}
		case Program::DeclKind::dkEnum:
			break;
		}
	}

	// Collect user function names.
	std::set<std::string> user_func_names;
	for (TokenFunc *tf : funcs)
		user_func_names.insert(tf->var.name);

	// Translate function bodies first, into a temp list, so referenced_funcs
	// (populated as N_CALL nodes are built) is complete before the prototype
	// pass — letting us emit extern protos for ONLY referenced functions.
	std::vector<node_t> func_def_nodes;
	for (TokenFunc *tf : funcs) {
		node_t fd = func_def(tf);
		if (fd) func_def_nodes.push_back(fd);
	}

	// Pass 0.75: Extern function prototypes — referenced-only (matches c2m,
	// which only declares what #include pulled in).
	for (auto &kv : prog->funcdef_map) {
		const std::string &fname = kv.first;
		FuncDef *fd = kv.second;
		if (!fd || user_func_names.count(fname)) continue;
		if (!referenced_funcs.count(fname)) continue;

		DataDef *ret_dd = &fd->returns;
		bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
		DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
		if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

		// Build: EXTERN + type spec
		node_t ext_list = list();
		append(ext_list, simple(N_EXTERN));
		if (ret_dd && ret_dd->is_struct()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(ret_dd);
			if (sdd)
				append(ext_list, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
			else
				append_type_specs(ext_list, ret_dd);
		} else {
			append_type_specs(ext_list, ret_dd);
		}
		node_t share = node1(N_SHARE, ext_list);

		// Parameters
		node_t param_list = list();
		if (fd->parameters.empty() && fd->is_void_params) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		} else {
			size_t nparam = fd->parameters.size();
			if (fd->is_varargs && nparam > 0) nparam--;
			for (size_t i = 0; i < nparam; i++) {
				char pname[16];
				snprintf(pname, sizeof(pname), "p%zu", i);
				append(param_list, param_decl(fd->parameters[i], pname));
			}
		}
		if (fd->is_varargs)
			append(param_list, simple(N_DOTS));

		node_t func_inner = node1(N_FUNC, param_list);
		node_t func_id_node = id(fname.c_str());
		node_t decl_list = list();
		append(decl_list, func_inner);
		if (ret_is_ptr)
			append(decl_list, pointer());

		node_t decl = node2(N_DECL, func_id_node, decl_list);

		node_t proto = simple(N_SPEC_DECL);
		append(proto, share);
		append(proto, decl);
		append(proto, ignore());
		append(proto, ignore());
		append(proto, ignore());
		append(top_list, proto);
	}

	// Pass 1: Forward declarations
	for (TokenFunc *tf : funcs) {
		if (tf->var.name == "main") continue;
		node_t proto = func_proto(tf);
		if (proto) append(top_list, proto);
	}

	// Pass 2: Function definitions (translated above).
	for (node_t fd : func_def_nodes)
		append(top_list, fd);

	append(module, top_list);
	return module;
}

// ---------------------------------------------------------------------------
// cir_dump_nodes — madc-owned walker over the cir_node tree.
// ---------------------------------------------------------------------------
// Prints our enhanced CIR tree directly (not via c2mir's dumper): node type,
// literal payload, and the +madc fields (source position, typedef alias).
// Leaf nodes are codes <= N_ID (payload lives in the union); every other
// node iterates its u.ops children. Every node in a CirBuilder tree is an
// arena-allocated cir_node, so CIR_NODE() is always valid here.

static void cir_dump_node(FILE *f, node_t n, int indent)
{
	if (!n) return;
	for (int i = 0; i < indent; i++) fputc(' ', f);
	fprintf(f, "%s", c2mir_node_code_name((c2mir_node_code_t)n->code));

	switch (n->code) {
	case N_I: case N_L:  fprintf(f, " %lld", (long long)n->u.l); break;
	case N_LL:           fprintf(f, " %lld", (long long)n->u.ll); break;
	case N_U: case N_UL: fprintf(f, " %llu", (unsigned long long)n->u.ul); break;
	case N_ULL:          fprintf(f, " %llu", (unsigned long long)n->u.ull); break;
	case N_F:            fprintf(f, " %g", (double)n->u.f); break;
	case N_D:            fprintf(f, " %g", n->u.d); break;
	case N_CH: case N_CH16: case N_CH32: fprintf(f, " '%d'", (int)n->u.ch); break;
	case N_STR: case N_STR16: case N_STR32:
		fprintf(f, " \"%s\"", n->u.s.s ? n->u.s.s : ""); break;
	case N_ID:           fprintf(f, " %s", n->u.s.s ? n->u.s.s : ""); break;
	default: break;
	}

	cir_node *cn = CIR_NODE(n);
	if (cn->typedef_name) fprintf(f, "  [typedef=%s]", cn->typedef_name);
	if (cn->src_file) fprintf(f, "  @%s:%d", cn->src_file, cn->src_line);
	fputc('\n', f);

	if (n->code > N_ID) {
		for (int i = 0; ; i++) {
			node_t op = c2mir_node_op(n, i);
			if (!op) break;
			cir_dump_node(f, op, indent + 2);
		}
	}
}

void cir_dump_nodes(FILE *f, node_t tree)
{
	fprintf(f, "=== CIR NODE TREE (+madc) ===\n");
	cir_dump_node(f, tree, 0);
	fprintf(f, "=== END CIR NODE TREE ===\n");
}
