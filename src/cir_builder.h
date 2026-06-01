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

	// True while translating the body of a function that returns std::string BY
	// VALUE. Such a function is lowered to the struct-return (__retbuf) ABI: its
	// C return type is `void`, a hidden `struct string *__retbuf` is its first
	// parameter, and `return s;` becomes "copy-construct *__retbuf from s; return;".
	// This avoids the double-free that a bitwise `return s;` would cause (the
	// local's cleanup dtor would free a buffer already shallow-copied into the
	// caller's return slot). Matches g++'s by-value class-return ABI.
	bool m_cur_func_returns_string = false;
	// Non-NULL while translating the body of a function that returns a
	// NON-TRIVIAL user class (one with object members / a dtor) BY VALUE. Such a
	// class uses the SAME struct-return (__retbuf) ABI as std::string: C return
	// type `void`, a hidden `struct Cls *__retbuf` first param, and `return obj;`
	// becomes "copy-construct *__retbuf from obj (member-wise); return;" — so the
	// returned object is deep-copied instead of bit-copied (which would
	// double-free the shared string buffer at both scope exits). A TRIVIAL struct
	// keeps c2mir's native struct return (no dtor -> bit-copy is safe).
	DataDefCLASS *m_cur_func_returns_object = NULL;
	// Pointee user-class while translating the body of a function returning a
	// `Cls *` (a real user class pointer), else NULL. Lets translate_return emit
	// an explicit derived->base upcast on `return <Derived*>;` (P2.6).
	DataDefCLASS *m_cur_func_returns_class_ptr = NULL;
	// Scalar C return type of the current function (non-NULL while translating a
	// body whose C return type is a plain scalar — int/long/pointer/double — not
	// void and not a __retbuf/string/object return). Lets translate_return supply
	// a typed zero for a gcc-accepted bare `return;` in a non-void function
	// (gnu89/c11 warn but accept it; the returned value is indeterminate, so a
	// zero of the right type is a conformant lowering c2mir will compile).
	DataDef *m_cur_func_scalar_ret = NULL;
	// Name of the hidden return-slot pointer parameter for a by-value class return.
	static const char *RETBUF_NAME;
	// Build the `struct <Cls> *__retbuf` named parameter node (N_SPEC_DECL), the
	// hidden first parameter of a by-value object-returning function. `retdd` is
	// the returned class/struct type (ddSTRING for a string-returning fn).
	node_t retbuf_param(DataDef *retdd, TokenBase *origin);

	// Statements that must be emitted in the enclosing block immediately
	// BEFORE the statement currently being translated — used to materialize
	// temporary runtime objects (e.g. a std::string built from a literal that
	// is passed to a function expecting a string object). translate_block
	// flushes this buffer ahead of each statement. Mirrors the old transpiler's
	// emit_ns_arg statement-level temp construction.
	std::vector<node_t> m_pending_stmts;
	int m_strtmp_counter = 0;
	// File-scope class-instance globals (std::string etc.) lower to opaque
	// struct storage at file scope plus a constructor call that must run before
	// any user code. C++ does this via static-init; madc injects these ctor
	// calls as the first statements of main's body (declaration order). Populated
	// by collect_global_ctors (translate_module), consumed by func_def for main.
	std::vector<node_t> m_global_ctor_stmts;
	// Names of the functions whose bodies madc COMPILES this module (the user's
	// TokenFuncs). Set in translate_module while bodies are translated; NULL
	// otherwise. Gates the by-value string-return (__retbuf) ABI to madc-compiled
	// functions only — an external / native function with a std::string return
	// type (e.g. __std_to_string) keeps its own ABI and must NOT be rewritten.
	const std::set<std::string> *m_user_func_names = nullptr;

	// GNU nested-function capture lowering. A nested function (`T inner(args){...}`
	// defined inside another function) that references an enclosing local/param
	// implicitly captures it BY REFERENCE — modelled exactly like a [&] lambda
	// capture: each used enclosing variable becomes a hidden pointer parameter
	// `T *name`, every body reference reads/writes through it (`(*name)`), and
	// every call site forwards `&var`. While translating a nested function's
	// body, m_cur_capture_set holds the enclosing Variables it MAY capture (its
	// FuncDef::potential_captures, by pointer identity); m_cur_captured_fd is the
	// FuncDef being filled. A var reference that hits the set is recorded in
	// FuncDef::captured_vars (deterministic first-use order) and emitted as a
	// deref of the same-named pointer parameter. NULL/empty outside a nested body.
	FuncDef *m_cur_captured_fd = nullptr;
	std::set<Variable *> m_cur_capture_set;
	// Record `v` as a capture of the current nested function (idempotent) and
	// return true when `v` is a captured variable of it. False when not nested
	// or `v` is local/param to the nested function itself.
	bool note_capture(Variable *v);

	// Internal: allocate and initialize a cir_node
	cir_node *make(c2mir_node_code_t code, TokenBase *origin = NULL);

	// Explicit pointer stars at a usage site = total pointer depth of the
	// declared type minus the typedef's own base depth. Returns -1 when
	// alias is empty (caller falls back to the non-typedef pointer path).
	int explicit_star_count(DataDef *full_type, const std::string &alias);

	// Build one N_MEMBER node for a struct/union member (shared by struct_def
	// and the inline-struct path in typedef_decl).
	node_t member_node(const memberpair_t &m, DataDefSTRUCT *owner = NULL);

	// Build the N_LIST of N_MEMBER nodes for an anonymous aggregate's body.
	node_t anon_members_list(DataDefSTRUCT *anon);
	// Build the inline type-spec for an anonymous aggregate: a one-element
	// LIST holding STRUCT/UNION(IGNORE-tag, members). An anonymous aggregate
	// has no tag to forward-reference, so its body must be emitted inline at
	// every declarator that uses it. Shared by var_decl's value/pointer/
	// static/extern paths.
	node_t anon_inline_spec(DataDefSTRUCT *anon);

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
	static bool is_string_object(DataDef *dd);   // std::string value type, not a pointer
	// True only for a genuine string OBJECT value (declared string variable /
	// string-returning expression) — EXCLUDES string literals (ttString tokens
	// and lifted `__literal__` vars), which are already const char* values.
	static bool is_string_object_value(TokenBase *arg);
	// True for a std::string `operator+` expression (`a + b`, `a + "lit"`,
	// chained `a+b+c`): a tkAdd whose LHS is a string object. Such an expression
	// is a by-value string object materialized by class_operator_call.
	static bool is_string_operator_plus(TokenBase *arg);
	// A CALL to a madc-COMPILED function whose callee returns a std::string
	// OBJECT by value (non-pointer) — i.e. one lowered through the
	// __retbuf ABI by func_def. Excludes external / native functions with a
	// std::string return (they keep their own ABI). Such a call is a string-object
	// rvalue that must be materialized into a scope temp before use.
	bool is_string_returning_call(TokenBase *arg);
	// A CALL to a madc-COMPILED function returning a NON-TRIVIAL user class by
	// value (one routed through the __retbuf ABI). Returns the class, or NULL.
	DataDefCLASS *object_returning_call_class(TokenBase *arg);
	// The user class that, returned by value, must use the __retbuf ABI (a
	// non-trivial class needing a dtor). NULL for std::string (its own path) and
	// trivial structs (native struct return). See cir_builder.cpp.
	DataDefCLASS *class_return_via_retbuf(DataDef *dd);
	// Member-wise copy-construct `cdd`'s object members from `src` into *__retbuf
	// (after a bit-copy for scalars), so the return slot owns its own buffers.
	void class_copy_construct_into_retbuf(DataDefCLASS *cdd, TokenBase *src,
					      std::vector<node_t> &out,
					      TokenBase *origin);
	// Member-wise copy-ASSIGNMENT of a non-trivial class into an existing object
	// (`lhs = rhs`): each member is copy-assigned (string -> string_assign,
	// scalar -> plain =), NOT bit-copied — avoids aliasing object members' heap
	// buffers (double-free). Self-assignment-safe. The implicit g++ operator=.
	// `class_copy_assign` takes an lvalue rhs; `class_copy_assign_from_addr`
	// takes a pre-materialized (void*) rhs address (a call temp evaluated once).
	void class_copy_assign(DataDefCLASS *cdd, TokenBase *lhs, TokenBase *rhs,
			       std::vector<node_t> &out, TokenBase *origin);
	void class_copy_assign_from_addr(DataDefCLASS *cdd, TokenBase *lhs,
					 node_t rhs_addr, std::vector<node_t> &out,
					 TokenBase *origin);
	void class_copy_assign_members(DataDefCLASS *cdd, const char *lname,
				       const char *rname, std::vector<node_t> &out,
				       TokenBase *origin);
	node_t class_ptr_bind(DataDefCLASS *cdd, const char *nm, node_t init,
			      TokenBase *origin);
	// Materialize an object-returning CALL (non-trivial class) into a
	// cleanup-tagged temp of that class via the __retbuf ABI, and return the
	// temp's (void*) address. Mirrors string_call_temp_addr for user classes.
	node_t object_call_temp_addr(TokenBase *call_tok, DataDefCLASS *cdd,
				     TokenBase *origin);
	// Materialize a string-returning CALL into a cleanup-tagged `struct string`
	// temp initialized directly by the call (the struct-return slot IS the temp,
	// matching g++ NRVO), and return the temp's (void*) address. Pushes the temp
	// decl to m_pending_stmts. `call` is the already-translated N_CALL node.
	node_t string_call_temp_addr(TokenBase *call_tok, TokenBase *origin);
	// Allocate a cleanup-tagged `struct string` temp (raw storage, no ctor — the
	// caller fills it via a return-slot/placement write) and push its decl to
	// m_pending_stmts. Returns the temp's name (into name_buf, size buf_sz).
	// Shared by string_call_temp_addr (__retbuf ABI) and the by-value operator+
	// path (string_concat out-slot).
	void string_temp_decl(char *name_buf, size_t buf_sz, TokenBase *origin);
	// Translate a TokenCallFunc's explicit arguments into `args` (a LIST node),
	// applying string-object / numeric-reference parameter coercion. Shared by
	// the normal call path and the string-return-temp materialization.
	void build_call_args(class TokenCallFunc *tcf, node_t args);
	size_t string_obj_words() const;             // ceil(sizeof(std::string)/sizeof(long))
	// Words of opaque storage for a runtime-object class (std::string) that has
	// a concrete ABI size but no madc data members. 0 for an ordinary user class.
	size_t object_class_words(DataDefCLASS *cdd) const;
	node_t void_ptr_type();                      // N_TYPE node for a (void*) cast
	node_t char_ptr_type();                      // N_TYPE node for a (char*) cast
	node_t ptr_type_node(DataDef *dd);           // N_TYPE node for an arbitrary pointer DataDef
	node_t class_ptr_type(DataDefCLASS *cdd);    // N_TYPE node for a (struct Cls *) cast
	// Derived->base pointer conversion (single inheritance, base subobject at
	// offset 0): if `lhs_dd` is `Base*` and the RHS expression `rhs` yields a
	// `Derived*` (Derived derives from Base, not equal), wrap `value` in an
	// explicit `(Base*)` cast so the emitted C is clean (no c2mir "incompatible
	// pointer" warning). Returns `value` unchanged otherwise. Single inheritance
	// only — offset 0.
	node_t upcast_class_ptr(node_t value, DataDef *lhs_dd, class TokenBase *rhs,
				class TokenBase *origin);
	node_t string_storage_decl(const char *name, TokenBase *origin); // long name[W];
	node_t string_obj_addr(const char *name, TokenBase *origin);     // (void*)&name (struct local)
	node_t string_var_addr(const Variable &v, TokenBase *origin);    // honours pointer-stored params
	// Raw object-instance address of a NAMED variable (not void*-cast): the
	// pointer itself when pointer-stored (by-value/by-ref param, `T*`), else
	// `&name`. Shared addressing rule for every object-class receiver.
	node_t object_var_addr(const class Variable &v, TokenBase *origin);
	node_t string_ctor_call(const char *name, TokenBase *initexpr, TokenBase *origin);
	// string_cstr((void*)obj) — coerce a std::string object argument to a
	// const char* (the std::string->const char* coercion) for a char*-expecting call.
	node_t string_cstr_arg(TokenBase *arg);
	// Coerce an argument to a `std::string` OBJECT pointer for a call whose
	// parameter is a std::string object (value or reference). A genuine string
	// object is passed by address directly; any const char* value (literal,
	// char* var) is materialized into a scope-lived temporary std::string
	// (storage + ctor pushed to m_pending_stmts, destructed via cleanup attr).
	node_t string_obj_arg(TokenBase *arg);

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
	// `obj[i]` on a user class defining `operator[]` -> the method call,
	// deref'd (operator[] returns T& == a T*), so it is a read/write lvalue.
	node_t class_subscript_call(class TokenSubscript *tsub, TokenBase *origin);
	// The BARE operator[] call (no deref) — for a T&-returning operator[] this
	// is the element ADDRESS (a T*). Used to take a string element's address.
	node_t class_subscript_addr(class TokenSubscript *tsub, TokenBase *origin);
	// True when `arg` is `obj[i]` on a class whose operator[] yields a string
	// OBJECT element (so the element is a real std::string reached by address).
	static bool is_string_subscript(TokenBase *arg);
	// True when `arg` is `base[i]` where base is a raw `string*` pointer or a
	// `string[]` fixed array — i.e. a string OBJECT element of a plain array,
	// not an operator[] container. The element is a real std::string reached by
	// the address `&base[i]`. (Distinct from is_string_subscript, which is the
	// operator[]-container case.)
	static bool is_string_array_subscript(TokenBase *arg);

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
	// Symbol name a (global) variable emits as. For a variable declared with
	// __attribute__((alias("target"))), this resolves the alias chain to the
	// target's name so every reference, and an &-of, names the real defined
	// symbol — the C-level `#define b a` identity (c2mir/MIR has no symbol-alias
	// primitive, so the alias is resolved here at the cir layer). Non-aliased
	// variables return their own name.
	std::string var_emit_name(const class Variable &v) const;
	node_t integer(long val, TokenBase *origin = NULL);
	// Type-aware integer literal: pick the c2mir literal node code
	// (N_I/N_U/N_L/N_UL) from the literal's own DataDef so a suffixed
	// constant (e.g. `0xffffffffull`) carries its real signedness/width
	// into c2mir's usual-arithmetic-conversion logic.
	node_t integer_typed(int64_t val, DataDef *dd, TokenBase *origin = NULL);
	node_t real(double val, TokenBase *origin = NULL);
	node_t real_float(float val, TokenBase *origin = NULL);
	node_t complex_literal(double val, DataDef *complex_dd, TokenBase *origin = NULL);
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
	// When `fd` returns an object BY VALUE through the __retbuf ABI (void
	// return + hidden `T* __retbuf` first param — std::string or a non-trivial
	// class), report that returned type so the fn-ptr type renders the same
	// ABI (`void (*)(T*, params)`) and indirect calls pass a retbuf temp.
	// Returns NULL for the ordinary (scalar/pointer/trivial) return shape.
	DataDef *fnptr_retbuf_type(class FuncDef *fd);
	// Extra pointer stars an fn-ptr usage carries beyond its typedef alias:
	// `DO_FUN *m` (alias is a function typedef) -> 1; `UNOP m` (alias already
	// a pointer-to-function typedef) -> 0. Returns 1 when the alias is unknown.
	int fnptr_alias_stars(const std::string &alias);

	// ---- Declaration builders ----
	// Recursively build an initializer value node: a scalar expression, or
	// for a nested brace element (TokenStructLit) a LIST(INIT(LIST(), val), ...).
	node_t init_value(TokenBase *elem, bool target_is_aggregate = false);
	// True when positional slot `idx` of a brace initializer for aggregate
	// type `dd` targets a member/element that is itself an aggregate (a
	// fixed array or nested struct/union) — used so a designated-init GAP on
	// that slot emits a nested zero-init `{}` instead of a scalar 0.
	bool init_slot_is_aggregate(DataDef *dd, size_t idx);
	// C99 compound literal `(T){ init... }` -> N_COMPOUND_LITERAL(type, list).
	node_t translate_struct_lit(class TokenStructLit *slit);
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
	// Lower a class method bound to an external symbol (FuncDef::emit_symbol —
	// a mangled libstdc++ std::string member): declares the extern from the
	// method's real signature and routes the call directly. `empty()` lowers
	// to `size()==0`. `this_arg` is the receiver address (object_var_addr).
	node_t emit_symbol_method_call(class TokenMember *tm, class FuncDef *callee,
				       const std::string &sym, node_t this_arg,
				       TokenBase *origin);
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
	// Select the operator overload (operator= / operator+=) matching the RHS:
	// a string-object RHS picks the (const string&) overload, a const char*
	// value picks the (const char*) overload. Falls back to the first by-name
	// match. NULL when the class has no such operator.
	class FuncDef *select_operator_overload(DataDefCLASS *cls,
				const std::string &mname, TokenBase *rhs);
	// Lower an overloaded binary operator on a user-defined class lvalue:
	//   c <op> rhs  ->  ClassName__operator<op>(&c, rhs)
	// when c's class defines a matching operator method. Returns NULL when
	// the left operand is not a user class or has no such operator (caller
	// falls through to the built-in operator translation).
	node_t class_operator_call(class TokenOperator *top, TokenBase *origin);
	// Lower an overloaded UNARY operator on a user-defined class lvalue:
	//   <op>c  ->  ClassName__operator<op>(&c)   (e.g. -c, !c, ~c, ++c, --c)
	// `opsym` is the operator symbol text ("-", "!", "~", "++", "--"); `operand`
	// is the class object the operator is applied to. Returns NULL when the
	// operand is not a class declaring that unary operator (caller falls through
	// to the built-in unary translation). The class declares the operator with
	// NO explicit parameter (param 0 = __this only) — that arity distinguishes
	// a unary operator- from the binary operator-(const C&).
	node_t class_unary_operator_call(const char *opsym, TokenBase *operand,
					 TokenBase *origin);
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
	// Scan file-scope class-instance globals (std::string and friends) that are
	// not source-declared via top_decls (built-ins like `version`, registered
	// programmatically), emit their opaque struct storage into `top_list`, and
	// queue each one's constructor call into m_global_ctor_stmts so func_def can
	// run them before main's body. Also queues ctor calls for source-declared
	// class globals (their storage already rode in via the dkGlobalVar pass).
	void collect_global_ctors(Program *prog,
				  std::vector<node_t> &deferred_globals,
				  std::set<std::string> &emitted_globals);
	// Build the constructor-call statement for a file-scope class global `v`
	// (type `cdd`), sourcing its initializer from the linked TokenDecl when
	// present (user source) or from v->data (a const char* literal, built-ins).
	node_t global_ctor_call(class Variable *v, DataDefCLASS *cdd, TokenDecl *decl);

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
	// SJLJ exception lowering: try/catch -> setjmp on __madc_try_push(&ctx), the
	// try body on the ==0 arm (then __madc_try_pop), the catch dispatch on the
	// else arm (by __madc_exception_type, bind value, run handler, clear; no
	// match -> __madc_rethrow). Emitted as block-scoped statements (NOT a stmt-
	// expr — see the c2mir cleanup-scope gotcha). throw -> __madc_throw_*.
	node_t translate_try(class TokenTRY *tt);
	node_t translate_throw(class TokenTHROW *th);
	int m_try_ctx_counter = 0;
	// >0 while lowering a try BODY (set around translate_stmt(tt->try_body) in
	// translate_try). Objects constructed in a try body keep their normal
	// cleanup-attribute (normal-path scope-exit teardown), but ALSO get a runtime
	// __madc_cleanup_push so __madc_throw_* unwinds them on the longjmp path
	// (cleanup attributes do NOT fire on longjmp — P1.1c). On normal try exit the
	// runtime entries are discarded (cleanup-attribute already ran the dtors).
	int m_try_body_depth = 0;
	// When in a try body, append a runtime cleanup entry naming `cdd`'s destructor
	// for the object `varname` (its dtor runs on the exception/longjmp unwind
	// path). No-op outside a try body or for a class with no dtor. See P1.1c
	// (docs/plans/refs/exceptions-sjlj.md).
	void emit_try_body_cleanup_push(const char *varname, class DataDefCLASS *cdd,
					node_t items, class TokenBase *origin);
	// Range-based for over a MadArray: `for (T x : arr) body`. The loop
	// variable is declared in the enclosing scope by the parser, so this only
	// emits the index loop + per-iteration element fill (php_array_get /
	// php_array_get_int) around the translated body.
	node_t translate_foreach(class TokenFOREACH *fe);
	// Range-for over a user-defined class / template-instantiated container:
	// `for (T x : c) body` -> index loop using c.size() and c[__i] (the
	// class's size()/operator[] methods). The loop var is declared in the
	// enclosing scope by the parser.
	node_t translate_foreach_class(class TokenFOREACH *fe,
				       class DataDefCLASS *cls,
				       class Variable *szmv, class Variable *opmv);
	// Range-for over a raw fixed-size C array: `for (T x : a) body` -> a plain
	// indexed loop over the array's compile-time element count with a direct
	// subscript `a[__i]`. (No MadArray runtime helper.)
	node_t translate_foreach_carray(class TokenFOREACH *fe, class TokenVar *ctv);
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
