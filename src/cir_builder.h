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

	// True while translating the body of a T&-returning function, so
	// translate_return emits `return &<expr>` (the reference is the address;
	// the call site derefs it). Matches g++: a reference IS a pointer.
	bool m_cur_func_returns_ref = false;

	// Statements that must be emitted in the enclosing block immediately
	// BEFORE the statement currently being translated — used to materialize
	// temporary runtime objects (e.g. a std::string built from a literal that
	// is passed to a function expecting a string object). translate_block
	// flushes this buffer ahead of each statement. Mirrors the old transpiler's
	// emit_ns_arg statement-level temp construction.
	std::vector<node_t> m_pending_stmts;
	int m_strtmp_counter = 0;

	// Internal: allocate and initialize a cir_node
	cir_node *make(c2mir_node_code_t code, TokenBase *origin = NULL);

	// Explicit pointer stars at a usage site = total pointer depth of the
	// declared type minus the typedef's own base depth. Returns -1 when
	// alias is empty (caller falls back to the non-typedef pointer path).
	int explicit_star_count(DataDef *full_type, const std::string &alias);

	// Build one N_MEMBER node for a struct/union member (shared by struct_def
	// and the inline-struct path in typedef_decl).
	node_t member_node(const memberpair_t &m, DataDefSTRUCT *owner = NULL);

	// ---- Opaque C++ runtime-object lowering (shared mechanism) ----
	// A monomorphic C++ object (std::string, MadArray, …) lowers to an 8-aligned
	// `long name[words]` buffer tagged with __attribute__((cleanup(dtor))) plus a
	// constructor call to a runtime wrapper in madc_mir_backend.cpp. One C++ decl
	// fans out (1->N) into these lowered nodes; all share the originating
	// TokenDecl in cir_node::origin and set synth_from_origin.
	// See docs/superpowers/plans/2026-05-30-cir-stdstring-lowering.md.
	node_t obj_storage_decl(const char *name, size_t words,
				const char *dtor_sym, TokenBase *origin);
	node_t obj_default_ctor_call(const char *name, const char *ctor_sym,
				     TokenBase *origin);

	// ---- std::string object lowering ----
	static bool is_string_object(DataDef *dd);   // dtSTRING value type, not a pointer
	// True only for a genuine string OBJECT value (declared string variable /
	// string-returning expression) — EXCLUDES string literals (ttString tokens
	// and lifted `__literal__` vars), which are already const char* values.
	static bool is_string_object_value(TokenBase *arg);
	size_t string_obj_words() const;             // ceil(sizeof(std::string)/sizeof(long))
	// Words of opaque storage for a runtime-object class (std::string) that has
	// a concrete ABI size but no madc data members. 0 for an ordinary user class.
	size_t object_class_words(DataDefCLASS *cdd) const;
	node_t void_ptr_type();                      // N_TYPE node for a (void*) cast
	node_t string_storage_decl(const char *name, TokenBase *origin); // long name[W];
	node_t string_obj_addr(const char *name, TokenBase *origin);     // (void*)name
	node_t string_ctor_call(const char *name, TokenBase *initexpr, TokenBase *origin);
	// string_cstr((void*)obj) — coerce a std::string object argument to a
	// const char* (the dtSTRING->dtCHARptr coercion) for a char*-expecting call.
	node_t string_cstr_arg(TokenBase *arg);
	// Coerce an argument to a `std::string` OBJECT pointer for a call whose
	// parameter is a string object (dtSTRING/dtSTRINGref). A genuine string
	// object is passed by address directly; any const char* value (literal,
	// char* var) is materialized into a scope-lived temporary std::string
	// (storage + ctor pushed to m_pending_stmts, destructed via cleanup attr).
	node_t string_obj_arg(TokenBase *arg);
	// Lower a std::string method call on a string OBJECT receiver to the
	// matching runtime wrapper: s.c_str()/.length()/.size()/.empty()/.clear().
	// Returns NULL when `tm` is not a recognized string-object method call,
	// so translate_expr falls through to its generic member/call handling.
	node_t string_method_call(class TokenMember *tm, TokenBase *origin);

	// ---- MadArray (`array`) object lowering ----
	// Same opaque-object model as std::string, but an array argument needs no
	// const char* coercion — it is always passed by pointer and the long[]
	// buffer name decays to that pointer at the call site.
	static bool is_array_object(DataDef *dd);    // dtARRAY value type, not a pointer
	size_t array_obj_words() const;              // ceil(sizeof(MadArray)/sizeof(long))
	node_t array_storage_decl(const char *name, TokenBase *origin);
	node_t array_ctor_call(const char *name, TokenBase *origin);

	// ---- std::stringstream object lowering ----
	// Same opaque-object model. A `ss << a << b` chain lowers to a sequence of
	// streamout_*(sstream_ostream((void*)&ss), value) calls (the streamout_*
	// runtime wrappers take an ostream*; sstream_ostream applies the
	// multiple-inheritance base-offset adjustment). printstream(ss) takes the
	// object pointer directly (dtSSTREAM param -> void*).
	static bool is_sstream_object(DataDef *dd); // dtSSTREAM value type, not a pointer
	size_t sstream_obj_words() const;           // ceil(sizeof(std::stringstream)/sizeof(long))
	node_t sstream_storage_decl(const char *name, TokenBase *origin);
	node_t sstream_ctor_call(const char *name, TokenBase *origin);
	// `ss << a << b ...` on a stringstream OBJECT -> a comma-sequence of
	// streamout_*(sstream_ostream((void*)&ss), value) calls.
	node_t translate_sstream_chain(class TokenOperator *top, const char *ssname);

	// ---- STL container (vector/map/set) object lowering ----
	// Same opaque-object model as std::string / MadArray: an 8-aligned long[]
	// buffer + a ctor call, destructed via the cleanup attribute. The runtime
	// symbol family is selected by container kind + element/key/value types
	// (e.g. dtVECTOR + string element -> "vector_str_*"). The container is
	// always passed to its wrappers by address ((void*)&buffer).
	static bool is_container_object(DataDef *dd);
	// Fill ctor/dtor wrapper names + word count for a container value type.
	// Returns false when `dd` is not a (non-pointer) container.
	bool container_obj_info(DataDef *dd, const char *&ctor_sym,
				const char *&dtor_sym, size_t &words);
	node_t container_storage_decl(const char *name, DataDef *dd, TokenBase *origin);
	node_t container_ctor_call(const char *name, DataDef *dd, TokenBase *origin);
	// Lower a container method call (vector .push_back/.size/.at/...; map
	// .put/.get/.contains/.size; set .insert/.contains/.size) on a container
	// OBJECT receiver to its runtime wrapper. Returns NULL when not a
	// recognized container-object method call (caller falls through).
	node_t container_method_call(class TokenMember *tm, TokenBase *origin);
	// Lower a container subscript READ `c[i]` to its runtime getter. Returns
	// NULL when `tsub` is not a container subscript.
	node_t container_subscript_read(class TokenSubscript *tsub, TokenBase *origin);
	// `obj[i]` on a user class defining `operator[]` -> the method call,
	// deref'd (operator[] returns T& == a T*), so it is a read/write lvalue.
	node_t class_subscript_call(class TokenSubscript *tsub, TokenBase *origin);
	// The BARE operator[] call (no deref) — for a T&-returning operator[] this
	// is the element ADDRESS (a T*). Used to take a string element's address.
	node_t class_subscript_addr(class TokenSubscript *tsub, TokenBase *origin);
	// True when `arg` is `obj[i]` on a class whose operator[] yields a string
	// OBJECT element (so the element is a real std::string reached by address).
	static bool is_string_subscript(TokenBase *arg);
	// Lower a container subscript WRITE `c[i] = rhs` to its runtime setter.
	// Returns NULL when the assignment LHS is not a container subscript.
	node_t container_subscript_assign(class TokenOperator *top, TokenBase *origin);

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
	// ret_ptr=true -> returns void*, else void. ret_specs overrides the
	// return base type when non-empty (e.g. {N_LONG} for a long-returning
	// runtime fn); empty means the default base type N_VOID.
	void need_output_extern(const char *symbol, bool ret_ptr,
				const std::vector<ExternParam> &params,
				const std::vector<c2mir_node_code_t> &ret_specs
					= std::vector<c2mir_node_code_t>());
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
	// Emit a user-defined class as a plain C struct definition. Base-class
	// members are already flattened into the derived class's member list by
	// the parser (single inheritance), so this is a flat struct. A `void
	// *__vptr;` slot is prepended when the class (or a base) has virtual
	// methods, matching the parser's layout (which reserves offset 0).
	node_t class_struct_def(DataDefCLASS *cdd);
	// Emit a class's virtual-method dispatch table as a file-scope array of
	// type-erased function pointers in vtable_slot order:
	//   void *ClassName__vtable[] = { (void*)C__slot0, (void*)C__slot1, ... };
	// Each slot resolves to the most-derived override visible to this class
	// (findMethod walks class -> base). Returns NULL when the class has no
	// vtable. Must be emitted after the method prototypes it references.
	node_t class_vtable_def(DataDefCLASS *cdd);
	// Lower a user-defined class method call on a class OBJECT (or pointer)
	// receiver to a free-function call on the mangled method symbol with the
	// receiver address threaded as the hidden first `__this` argument:
	//   c.method(a, b)   -> ClassName__method(&c, a, b)
	//   p->method(a, b)  -> ClassName__method(p, a, b)
	// A virtual call dispatches through the receiver's __vptr slot instead of
	// naming the symbol directly. Returns NULL when the receiver is not a
	// user-defined class (caller falls through to generic member access).
	node_t class_method_call(class TokenMember *tm, TokenBase *origin);
	// Build the hidden `__this` argument for a class method/operator call:
	// the receiver's address for a value receiver, or the pointer itself for
	// a pointer receiver. `recv_class` is filled with the receiver's class.
	node_t class_this_arg(class TokenMember *tm, DataDefCLASS *&recv_class,
			      TokenBase *origin);
	// Build the constructor-call statement for a class instance `v`:
	//   ClassName__ClassName(&v, ctor_args...)
	// Returns NULL when `v` is not a user class or has no user constructor.
	node_t class_ctor_call(class Variable *v, DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args,
			       TokenBase *origin);
	// Select the ctor overload of `cdd` matching the initializer arguments
	// (default / const char* / copy for std::string; the single ctor for a
	// user class). NULL when no overload set is recorded.
	class FuncDef *select_ctor_overload(DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args);
	// Lower an overloaded binary operator on a user-defined class lvalue:
	//   c <op> rhs  ->  ClassName__operator<op>(&c, rhs)
	// when c's class defines a matching operator method. Returns NULL when
	// the left operand is not a user class or has no such operator (caller
	// falls through to the built-in operator translation).
	node_t class_operator_call(class TokenOperator *top, TokenBase *origin);
	// Build a function-pointer cast type node for a method's signature:
	// `RET (*)(struct Owner *, params...)`. Used to cast a type-erased vtable
	// slot back to a callable function pointer for virtual dispatch. The
	// callee's parameter 0 is already the (owner *) __this, so its full
	// parameter list is emitted as-is.
	node_t method_fnptr_type(class FuncDef *callee, DataDefCLASS *owner);
	// Append placement-new construct / destruct calls for every embedded
	// OBJECT member (std::string today) of `cdd` to `out`, addressing each
	// member through the in-scope `__this` pointer:
	//   string_construct((void*)&__this->member)  /  string_destruct(...)
	// Used to give a class with object members proper member lifetime inside
	// its (possibly synthesized) ctor/dtor. Returns true if it emitted any.
	bool class_member_construct(DataDefCLASS *cdd, std::vector<node_t> &out,
				    TokenBase *origin);
	bool class_member_destruct(DataDefCLASS *cdd, std::vector<node_t> &out,
				   TokenBase *origin);
	// True when the class has at least one embedded object member needing
	// construction/destruction (so it requires a ctor/dtor even if the user
	// wrote none).
	bool class_has_object_members(DataDefCLASS *cdd);
	// Append `string_construct((void*)&inst.member)` statements (one per
	// embedded object member) to the c2mir list node `items`. Used at a
	// value class-instance declaration that has object members but no user
	// constructor (the member access is `inst.member`, not `__this->member`).
	void class_instance_member_ctors(const char *inst, DataDefCLASS *cdd,
					 node_t items, TokenBase *origin);
	// True when a class needs an (implicit) destructor: it has a user dtor,
	// embedded object members, or a base class that needs one.
	bool class_needs_dtor(DataDefCLASS *cdd);
	// The destructor symbol used as the cleanup function for a class
	// instance (ClassName___dtor) — whether user-written or synthesized.
	std::string class_dtor_symbol(DataDefCLASS *cdd);
	// Emit a synthesized destructor function for a class that needs a dtor
	// (object members and/or a base dtor) but has no user-written one:
	//   void Class___dtor(struct Class *__this) {
	//       string_destruct(&__this->member); ...; Base___dtor((Base*)__this);
	//   }
	// Returns NULL when the class has a user dtor (its own def handles this)
	// or needs no dtor.
	node_t synth_dtor_def(DataDefCLASS *cdd);
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
	// Range-based for over a MadArray: `for (T x : arr) body`. The loop
	// variable is declared in the enclosing scope by the parser, so this only
	// emits the index loop + per-iteration element fill (php_array_get /
	// php_array_get_int) around the translated body.
	node_t translate_foreach(class TokenFOREACH *fe);
	// Range-for over a std::vector: `for (T x : vec) body`. Iterates by index
	// using vector_int_size/at (int element) or vector_str_size/at (string
	// element, filled into the enclosing-scope loop var). The loop variable is
	// declared in the enclosing scope by the parser (same as translate_foreach).
	node_t translate_foreach_vector(class TokenFOREACH *fe,
					class DataDefVECTOR *vdd);
	// Range-for over a user-defined class / template-instantiated container:
	// `for (T x : c) body` -> index loop using c.size() and c[__i] (the
	// class's size()/operator[] methods). The loop var is declared in the
	// enclosing scope by the parser.
	node_t translate_foreach_class(class TokenFOREACH *fe,
				       class DataDefCLASS *cls,
				       class Variable *szmv, class Variable *opmv);
	node_t translate_do(TokenDO *td);
	node_t translate_switch(TokenSWITCH *ts);
	// rust::match over integer patterns -> a switch: each arm's pattern(s)
	// become case labels (OR-list `a | b` -> multiple labels), `_` -> default,
	// and every arm ends with an implicit break (no fall-through).
	node_t translate_match(class TokenMatch *tm);

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
