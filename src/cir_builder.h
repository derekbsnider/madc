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
#include <map>
#include <vector>

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
class TokenOperator;
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
	// Global variables referenced while translating bodies. Used to emit
	// extern decls for libc globals (stderr/stdout/stdin, registered lazily
	// via addGlobal but absent from top_decls) so the emitted C compiles.
	std::map<std::string, Variable *> referenced_globals;
	std::map<std::string, node_t> m_output_externs; // symbol -> proto SPEC_DECL (dedup)
	std::set<std::string> m_stream_objects;       // dedup stream object externs
	std::vector<node_t>   m_stream_object_protos;  // extern object decls to emit
	// Set during translate_module. Used to resolve a typedef alias to its
	// base DataDef so we can tell the typedef's own pointer depth apart from
	// the explicit stars written at the usage site.
	Program *m_prog;
	// True while translating the body of a void-returning function — lets
	// translate_return lower a gcc-accepted `return <expr>;` to `<expr>;
	// return;` (c2mir rejects a value in a void return).
	bool m_cur_func_returns_void = false;

	// Internal: allocate and initialize a cir_node
	cir_node *make(c2mir_node_code_t code, TokenBase *origin = NULL);

	// Explicit pointer stars at a usage site = total pointer depth of the
	// declared type minus the typedef's own base depth. Returns -1 when
	// alias is empty (caller falls back to the non-typedef pointer path).
	int explicit_star_count(DataDef *full_type, const std::string &alias);

	// Build one N_MEMBER node for a struct/union member (shared by struct_def
	// and the inline-struct path in typedef_decl).
	node_t member_node(const memberpair_t &m, DataDefSTRUCT *owner = NULL);

	// ---- std::string object lowering ----
	// A madc `string` lowers to a real std::string OBJECT (Cfront-style): an
	// 8-aligned opaque buffer plus ctor/dtor calls to the runtime wrappers in
	// madc_mir_backend.cpp. One C++ decl fans out (1->N) into these lowered
	// nodes; all share the originating TokenDecl in cir_node::origin and set
	// synth_from_origin. See docs/superpowers/plans/2026-05-30-cir-stdstring-lowering.md.
	static bool is_string_object(DataDef *dd);   // dtSTRING value type, not a pointer
	size_t string_obj_words() const;             // ceil(sizeof(std::string)/sizeof(long))
	node_t void_ptr_type();                      // N_TYPE node for a (void*) cast
	node_t string_storage_decl(const char *name, TokenBase *origin); // long name[W];
	node_t string_obj_addr(const char *name, TokenBase *origin);     // (void*)name
	node_t string_ctor_call(const char *name, TokenBase *initexpr, TokenBase *origin);
	node_t string_dtor_call(const char *name, TokenBase *origin);
	// string_cstr((void*)obj) — coerce a std::string object argument to a
	// const char* (the dtSTRING->dtCHARptr coercion) for a char*-expecting call.
	node_t string_cstr_arg(TokenBase *arg);

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

	// ---- Function-pointer declarators ----
	// A fn-ptr type (DataDefFPTR) must render as `ret (*name)(params)`, not the
	// `long` its dtINT64 rawtype would yield. fnptr_func_node builds the
	// N_FUNC(params) suffix from the target signature; fnptr_decl_pieces appends
	// the target's return-type specifiers into `spec_list` and fills `decl_list`
	// with the c2m innermost-first suffix order
	// ([lead_dims..., POINTER, FUNC, ret-pointer stars...]).
	node_t fnptr_func_node(class FuncDef *fd);
	void fnptr_decl_pieces(class FuncDef *fd, bool emit_pointer,
			       node_t spec_list, node_t decl_list,
			       const std::vector<uint32_t> &lead_dims);
	// Extra pointer stars an fn-ptr usage carries beyond its typedef alias:
	// `DO_FUN *m` (alias is a function typedef) -> 1; `UNOP m` (alias already
	// a pointer-to-function typedef) -> 0. Returns 1 when the alias is unknown.
	int fnptr_alias_stars(const std::string &alias);

	// ---- Declaration builders ----
	// Recursively build an initializer value node: a scalar expression, or
	// for a nested brace element (TokenStructLit) a LIST(INIT(LIST(), val), ...).
	node_t init_value(TokenBase *elem);
	node_t var_decl(Variable *v, TokenBase *origin = NULL);
	node_t param_decl(DataDef *ptype, const char *pname,
			  const std::string &typedef_alias = std::string());

	// ---- Output (Phase-2) ----
	// A param of an output extern: its type-spec node codes + whether it is a pointer.
	struct ExternParam { std::vector<c2mir_node_code_t> specs; bool ptr; };
	// Record (once) an extern proto for an output runtime/libstdc++ symbol.
	// ret_ptr=true -> returns void*, else void.
	void need_output_extern(const char *symbol, bool ret_ptr,
				const std::vector<ExternParam> &params);
	// Map a builtin print-fn name to its madc_* runtime symbol ("" if not one).
	static const char *builtin_output_runtime(const std::string &name);

	// ---- Stream chains (cout << x) ----
	// PoC: lower a C++ stream chain to direct calls on the mangled
	// libstdc++ stream object via the mangled operator<< symbol.
	enum StreamKind { SK_NONE, SK_COUT, SK_CERR, SK_CLOG, SK_CIN };
	StreamKind stream_ident_kind(TokenBase *tb);
	static const char *stream_object_symbol(StreamKind k);
	void need_stream_object(StreamKind k);
	node_t translate_stream_chain(TokenOperator *top, StreamKind k, bool is_out);
	// Pick the mangled operator<< overload for a value's type; fill p_out with
	// the value param's spec/ptr shape for the extern proto.
	const char *ostream_insert_symbol(DataDef *dd, ExternParam &p_out);

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
	// Like translate_stmt, but for required-statement slots (loop/if
	// bodies) where c2mir demands a node. translate_stmt returns NULL for
	// an empty `;` (correctly dropped inside a block); a required slot
	// must instead receive c2mir's empty-statement node, EXPR(LIST,IGNORE).
	node_t translate_stmt_required(TokenBase *tb);
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
