/* cir_builder.h — Builder for cir_node AST trees.
 *
 * CirBuilder allocates cir_node objects from its arena, assigns uids
 * via c2mir_next_uid(), and builds c2mir-compatible trees that also
 * carry madc-specific data.  The resulting tree can be passed directly
 * to c2mir_compile_tree().
 */

#ifndef __CIR_BUILDER_H
#define __CIR_BUILDER_H 1

#include "cir_node.h"
#include <string>
#include <set>

// Forward declarations
class Program;
struct memberpair_t;
class TokenFunc;
class TokenCpnd;
class TokenBase;
class TokenIF;
class TokenFOR;
class TokenDO;
class TokenSWITCH;
class TokenCASE;
class TokenRETURN;
class Variable;
class DataDef;
class DataDefSTRUCT;
class FuncDef;

class CirBuilder {
	c2m_ctx_t c2m;
	CirArena arena;
	// Function names referenced (called) while translating bodies. Used by
	// translate_module to emit extern prototypes only for funcs the source
	// actually uses (matches c2m's #include-driven scope).
	std::set<std::string> referenced_funcs;
	// Set during translate_module. Used to resolve a typedef alias to its
	// base DataDef so we can tell the typedef's own pointer depth apart from
	// the explicit stars written at the usage site.
	Program *m_prog;

	// Internal: allocate and initialize a cir_node
	cir_node *make(c2mir_node_code_t code, TokenBase *origin = NULL);

	// Explicit pointer stars at a usage site = total pointer depth of the
	// declared type minus the typedef's own base depth. Returns -1 when
	// alias is empty (caller falls back to the non-typedef pointer path).
	int explicit_star_count(DataDef *full_type, const std::string &alias);

	// Build one N_MEMBER node for a struct/union member (shared by struct_def
	// and the inline-struct path in typedef_decl).
	node_t member_node(const memberpair_t &m);

	// Internal: set position on a node in c2mir's node_positions VARR
	void set_pos(cir_node *cn, const char *file, int line, int col);
	void set_pos(cir_node *cn, TokenBase *tb);

public:
	CirBuilder(c2m_ctx_t c2m_ctx);
	~CirBuilder();

	// Access the c2mir context (for c2mir_compile_tree etc.)
	c2m_ctx_t context() const { return c2m; }

	// ---- Leaf node builders ----
	node_t id(const char *name, TokenBase *origin = NULL);
	node_t integer(long val, TokenBase *origin = NULL);
	node_t real(double val, TokenBase *origin = NULL);
	node_t ch(long val, TokenBase *origin = NULL);
	node_t str(const char *s, size_t len, TokenBase *origin = NULL);
	node_t ignore();

	// ---- Composite node builders ----
	node_t list();
	node_t node1(c2mir_node_code_t code, node_t op1, TokenBase *origin = NULL);
	node_t node2(c2mir_node_code_t code, node_t op1, node_t op2, TokenBase *origin = NULL);
	node_t node3(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, TokenBase *origin = NULL);
	node_t node4(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, node_t op4, TokenBase *origin = NULL);
	node_t node5(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, node_t op4, node_t op5, TokenBase *origin = NULL);
	node_t append(node_t parent, node_t child);
	node_t simple(c2mir_node_code_t code, TokenBase *origin = NULL);

	// Build an error/incomplete node (carries a reason + origin for
	// diagnostics). The node's code is N_IGNORE so it is harmless if it ever
	// leaked, but cir_tree_has_error() / the pre-c2mir gate must reject any
	// tree containing one.
	node_t error_node(const char *reason, TokenBase *origin = NULL);

	// ---- Type builders ----
	// Build type specifier LIST. If typedef_alias is non-empty, emit
	// ID("alias") — c2mir's checker resolves it from the typedef SPEC_DECL.
	void append_type_specs(node_t list, DataDef *dd);
	node_t type_list(DataDef *dd, const std::string &typedef_alias = "");
	node_t pointer();

	// ---- Declaration builders ----
	node_t var_decl(Variable *v, TokenBase *origin = NULL);
	node_t param_decl(DataDef *ptype, const char *pname);
	node_t typedef_decl(const std::string &alias, DataDef *dd,
			    const std::set<std::string> &emitted_structs,
			    bool force_incomplete_struct = false);
	node_t struct_def(DataDefSTRUCT *sdd);
	node_t func_proto(TokenFunc *tf);
	node_t func_def(TokenFunc *tf);

	// ---- Expression translation ----
	node_t translate_expr(TokenBase *tb);

	// ---- Statement translation ----
	node_t translate_stmt(TokenBase *tb);
	node_t translate_block(TokenCpnd *tc);
	node_t translate_return(TokenRETURN *tr);
	node_t translate_if(TokenIF *ti);
	node_t translate_while(TokenBase *tw);
	node_t translate_for(TokenFOR *tf);
	node_t translate_do(TokenDO *td);
	node_t translate_switch(TokenSWITCH *ts);

	// ---- Top-level module translation ----
	node_t translate_module(Program *prog);
};

// Dump the cir_node tree (our own walker, not c2mir's): node types,
// literal payloads, and the +madc fields (source position, typedef
// alias). Used by --dump-nodes.
void cir_dump_nodes(FILE *f, node_t tree);

// Validity gate: true if any node in the tree is an error/incomplete node
// (error_msg set). A tree with errors must not be handed to c2mir.
bool cir_tree_has_error(node_t tree);

// Walk the tree and print every error node (@file:line:column + message) to f;
// returns the number of error nodes found. Used to gate compilation.
int cir_report_errors(FILE *f, node_t tree);

#endif // __CIR_BUILDER_H
