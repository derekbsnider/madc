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
class Method;

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
	std::set<std::string> m_rtti_data_externs;    // dedup extern data decls for RTTI (S5b)
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

	// Non-NULL while translating the body of a function that returns a
	// non-trivial class (one with object members / a dtor) BY VALUE. Such a
	// class uses the struct-return (__retbuf) ABI: C return type `void`, a
	// hidden `struct Cls *__retbuf` first param, and `return obj;`
	// becomes "copy-construct *__retbuf from obj (member-wise); return;" — so the
	// returned object is deep-copied instead of bit-copied (which would
	// double-free the shared string buffer at both scope exits). A TRIVIAL struct
	// keeps c2mir's native struct return (no dtor -> bit-copy is safe).
	DataDefCLASS *m_cur_func_returns_object = NULL;
	// Non-NULL while translating the body of a function that returns a
	// TRIVIALLY-COPYABLE class (no dtor -> c2mir's native struct return, NOT
	// the __retbuf ABI above) BY VALUE. Lets translate_return apply an implicit
	// converting constructor for return-value copy-initialization when the
	// returned expression's class differs from the return class
	// ([stmt.return]/[dcl.init]) — e.g. std::set's `iterator find(){ return
	// _M_t.find(x); }`, where the tree's `iterator` converts to set's
	// `const_iterator`. The non-trivial (__retbuf) path already applies the
	// converting ctor via class_copy_construct_into_retbuf's overload selection.
	DataDefCLASS *m_cur_func_returns_value_class = NULL;
	// Returned user-class while translating the body of a function returning a
	// `Cls *` or `Cls&`, else NULL. Lets translate_return emit the derived->base
	// adjustment for pointer and reference returns.
	DataDefCLASS *m_cur_func_returns_class_ptr = NULL;
	// Scalar C return type of the current function (non-NULL while translating a
	// body whose C return type is a plain scalar — int/long/pointer/double — not
	// void and not a __retbuf/string/object return). Lets translate_return supply
	// a typed zero for a gcc-accepted bare `return;` in a non-void function
	// (gnu89/c11 warn but accept it; the returned value is indeterminate, so a
	// zero of the right type is a conformant lowering c2mir will compile).
	DataDef *m_cur_func_scalar_ret = NULL;
	Method *m_cur_method = NULL;
	// True while translating the body of a multi-return function (`return a, b;`,
	// Go-style). Such a function uses the multi-return __retbuf ABI: C return type
	// `void`, a hidden `long *__retbuf` first parameter, and `return a, b;` becomes
	// "__retbuf[0]=a; __retbuf[1]=b; return;". The call site `x, y := f()` allocates
	// a `long __mret[N]` buffer, passes it as __retbuf, then reads x=__mret[0] etc.
	// (Ported to cir_builder 2026-06-02; it was an asmjit-only feature dropped when
	// that backend was removed in 64f44b3 and never reimplemented on CIR.)
	bool m_cur_func_multi_return = false;
	// Per-translation counter for unique multi-return buffer names (__mret_K).
	int m_mret_counter = 0;
	// Active `defer` scopes while translating the current function's body:
	// each entry is a compound whose `deferred` vector is non-empty, innermost
	// at the back. translate_block pushes/pops; a compound's fall-off end runs
	// its OWN scope's deferred statements, a `return` runs EVERY active
	// scope's (innermost scope first, each scope's list in reverse
	// registration = LIFO order). Deferred statements run BEFORE destructors:
	// dtors ride the c2mir cleanup attribute, which fires at the actual scope
	// exit after these inline statements. Matches the old backend's
	// TokenCpnd::cleanup() ordering.
	std::vector<class TokenCpnd *> m_defer_scopes;
	// Per-translation counter for unique defer return-value temp names.
	int m_defer_tmp_counter = 0;
	// Inputs to rebuild the current function's C return type at a return site
	// (a cir node is single-parent, so func_def's spec tree cannot be reused):
	// type_list(m_cur_func_ret_spec_dd[, m_cur_func_ret_spec_alias]) plus
	// m_cur_func_ret_stars pointer suffixes. NULL spec_dd = no hoistable
	// C return value (void / __retbuf / multi-return shapes).
	DataDef *m_cur_func_ret_spec_dd = NULL;
	std::string m_cur_func_ret_spec_alias;
	int m_cur_func_ret_stars = 0;
	// Append every pending deferred statement to `items`: scopes from the
	// innermost down to m_defer_scopes[from_scope] inclusive, each scope's
	// list in LIFO order. Flushes per-statement materialized temps.
	void append_deferred_stmts(node_t items, size_t from_scope);
	// Name of the hidden return-slot pointer parameter for a by-value class return.
	static const char *RETBUF_NAME;
	// Build the `struct <Cls> *__retbuf` named parameter node (N_SPEC_DECL), the
	// hidden first parameter of a by-value object-returning function. `retdd` is
	// the returned class/struct type.
	node_t retbuf_param(DataDef *retdd, TokenBase *origin);

	// Lower a multi-return call-site `a, b, ... := f(args)` (a TokenAssign whose
	// multi_vars holds the N target variables). Pushes the `long __mret_K[N]`
	// buffer decl, the `f(__mret_K, args)` call, and the assigns for multi_vars[1..]
	// to m_pending_stmts (flushed before this decl statement); returns __mret_K[0]
	// as multi_vars[0]'s initializer. See m_cur_func_multi_return.
	node_t multi_return_unpack(class TokenAssign *as, TokenBase *origin);

	// Statements that must be emitted in the enclosing block immediately
	// BEFORE the statement currently being translated — used to materialize
	// temporary runtime objects (e.g. a literal converted through a class
	// constructor for an object parameter). translate_block
	// flushes this buffer ahead of each statement. Mirrors the old transpiler's
	// emit_ns_arg statement-level temp construction.
	std::vector<node_t> m_pending_stmts;
	// Splice m_pending_stmts into `out` (preserving order) and clear it.
	// For statement-list builders that run OUTSIDE translate_block's
	// statement loop (ctor/dtor prologue + epilogue synthesis): a temp
	// materialized while building a prologue statement must be emitted
	// into the same prologue, before its consumer — without this it
	// leaked into the NEXT translated function's body.
	void flush_pending_stmts(std::vector<node_t> &out);
	int m_strtmp_counter = 0;
	// File-scope class-instance globals lower to opaque
	// struct storage at file scope plus a constructor call that must run before
	// any user code. C++ does this via static-init; madc injects these ctor
	// calls as the first statements of main's body (declaration order). Populated
	// by collect_global_ctors (translate_module), consumed by func_def for main.
	std::vector<node_t> m_global_ctor_stmts;
	// Names of the functions whose bodies madc COMPILES this module (the user's
	// TokenFuncs). Set in translate_module while bodies are translated; NULL
	// otherwise. Gates the by-value non-trivial-class return (__retbuf) ABI to
	// madc-compiled functions only; external/native functions keep their own ABI.
	const std::set<std::string> *m_user_func_names = nullptr;
	// Emit symbols of deferred lazy bodies ([temp.inst]) MATERIALIZED this
	// module: madc emits their definitions (retbuf ABI for by-value class
	// returns) but they are not in m_user_func_names; the classification in
	// object_returning_call_class needs them after deferred_lazy_bodies
	// erases the materialized entry.
	std::set<std::string> m_materialized_lib_syms;

	// Top-level typedef aliases that COLLIDE: the same bare alias is registered
	// by >1 namespace for DIFFERENT underlying struct tags (e.g. std::string and
	// std::pmr::string both alias the bare name `string`). Flat C would then get
	// two conflicting `typedef <tag> string;` -> c2mir "repeated declaration".
	// Populated once per module (translate_module); empty for the common case.
	std::set<std::string> m_ambiguous_typedef_aliases;
	// Struct-tag name -> the COMBINED `typedef struct Tag {...} Alias;` TopDecl that
	// is the tag's body-definition point. When emit_struct_with_deps must hoist such
	// a struct (a by-value member needs it complete early), it emits the WHOLE
	// combined decl (body + alias) so the Alias name is available to dependents
	// hoisted after it — not just the bare struct body (which would strand the alias
	// at its original, later position -> "unknown type Alias"). Populated in the
	// translate_module pre-scan; the aliases it emits early are recorded below so
	// the source-order Pass 0 skips re-emitting them.
	std::map<std::string, Program::TopDecl *> m_combined_typedef_alias;
	std::set<std::string> m_hoisted_combined_aliases;
	// The C identifier to EMIT for a typedef alias `alias` of type `dd`: normally
	// the bare alias itself, but for an ambiguous alias (above) backed by a struct
	// the already-unique struct tag instead, so std vs std::pmr stay distinct C
	// names. Bare-alias STORAGE is unchanged (datatype_map lookups still key on
	// the bare name); only emission of the type-spec id is rewritten.
	std::string typedef_emit_name(const std::string &alias, DataDef *dd) const;
	// Peel array/pointer layers to the DataDefSTRUCT a typedef ultimately names
	// (NULL if none). Shared by translate_module's typedef pass, the collision
	// detection, and typedef_emit_name.
	static DataDefSTRUCT *struct_behind(DataDef *dd);

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
	node_t anonymous_aggregate_member_node(DataDefSTRUCT *anon);

	// Build the N_LIST of N_MEMBER nodes for an aggregate body, preserving
	// anonymous nested aggregate groups as unnamed STRUCT/UNION members.
	node_t anon_members_list(DataDefSTRUCT *anon);
	// Build the inline type-spec for an anonymous aggregate: a one-element
	// LIST holding STRUCT/UNION(IGNORE-tag, members). An anonymous aggregate
	// has no tag to forward-reference, so its body must be emitted inline at
	// every declarator that uses it. Shared by var_decl's value/pointer/
	// static/extern paths.
	node_t anon_inline_spec(DataDefSTRUCT *anon);

	// ---- Opaque C++ runtime-object lowering (shared mechanism) ----
	// A monomorphic C++ object lowers to an 8-aligned
	// `long name[words]` buffer tagged with __attribute__((cleanup(dtor))) plus a
	// constructor call to a runtime wrapper in madc_mir_backend.cpp. One C++ decl
	// fans out (1->N) into these lowered nodes; all share the originating
	// TokenDecl in cir_node::origin and set synth_from_origin.
	// See docs/superpowers/plans/2026-05-30-cir-stdstring-lowering.md.
	node_t obj_storage_decl(const char *name, size_t words,
				const char *dtor_sym, TokenBase *origin,
				size_t align = 0);
	// Host-call shim synthesis (translate_module): a per-function
	// `long __madc_shim_<sym>(char *__args, char *__out)` adapter over
	// the 32-byte madc_value ABI. NULL when the signature is not
	// host-marshallable (the host then gets a clean unsupported error).
	node_t synth_call_shim(Program *prog, TokenFunc *tf);
	// The shim core, keyed on the function Variable — serves parsed
	// functions AND host-callback trampolines (which have no TokenFunc).
	node_t synth_call_shim_var(Program *prog, Variable *fvar);
	node_t ctor_call_assemble(node_t this_addr, DataDefCLASS *cdd,
				  FuncDef *ctor,
				  const std::vector<node_t> &explicit_nodes,
				  TokenBase *origin);
	void synth_call_shims(Program *prog, const std::vector<TokenFunc *> &roots,
			      std::vector<node_t> &func_def_nodes);
	// Host-callback trampoline synthesis (translate_module): one module
	// definition `RET name(params) { return __madc_host_cb_<k>(...); }`
	// per Program::HostCallbackReg — the reverse of synth_call_shim. The
	// import symbol is bound to the host entry by the JIT session's
	// import resolver at MIR link.
	node_t synth_host_trampoline(Program *prog,
				     const Program::HostCallbackReg &reg);
	void synth_host_trampolines(Program *prog,
				    std::vector<node_t> &func_def_nodes);
	node_t obj_default_ctor_call(const char *name, const char *ctor_sym,
				     TokenBase *origin);

	// ---- Generic class-object lowering ----
	static bool is_class_object(DataDef *dd);   // class value type, not a pointer
	// True only for a genuine class OBJECT value (declared class variable,
	// class member, class-array element, or reference/value parameter).
	static bool is_class_object_value(TokenBase *arg);
	// A CALL to a madc-COMPILED function returning a non-trivial class by value
	// (one routed through the __retbuf ABI). Returns the class, or NULL.
	DataDefCLASS *object_returning_call_class(TokenBase *arg);
	// The referenced type of a REFERENCE-returning call argument
	// (std::move(x), a T&/T&& method) — the pointer representation
	// unwrapped; NULL when arg is not a ref-returning call.
	DataDef *ref_returning_call_type(TokenBase *arg);
	// The class that, returned by value, must use the __retbuf ABI (a
	// non-trivial class needing a dtor). NULL for trivial structs (native
	// struct return). See cir_builder.cpp.
	DataDefCLASS *class_return_via_retbuf(DataDef *dd);
	// Member-wise copy-construct `cdd`'s object members from `src` into *__retbuf
	// (after a bit-copy for scalars), so the return slot owns its own buffers.
	void class_copy_construct_into_retbuf(DataDefCLASS *cdd, TokenBase *src,
					      std::vector<node_t> &out,
					      TokenBase *origin);
	// Member-wise copy-ASSIGNMENT of a non-trivial class into an existing object
	// (`lhs = rhs`): each class member uses its registered assignment operator
	// when available; scalar members use plain assignment. This avoids aliasing
	// object members' owned resources. Self-assignment-safe. The implicit g++
	// operator=.
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
	// temp's (void*) address.
	node_t object_call_temp_addr(TokenBase *call_tok, DataDefCLASS *cdd,
				     TokenBase *origin);
	// Allocate a cleanup-tagged object temp (raw storage, no ctor) and push its
	// decl to m_pending_stmts. Returns the temp's name through name_buf.
	void object_temp_decl(DataDefCLASS *cdd, char *name_buf, size_t buf_sz,
			      TokenBase *origin);
	// Translate a TokenCallFunc's explicit arguments into `args` (a LIST node),
	// applying object / numeric-reference parameter coercion. Shared by the
	// normal call path and by-value object-return temp materialization.
	// `param_base` is the index in the callee's `parameters` of the FIRST
	// explicit user argument: 0 for a free/static function, 1 for a method
	// (whose `parameters[0]` is the hidden __this). The caller injects __this
	// itself; without the right base the per-arg coercion reads the wrong
	// formal (e.g. __this) and a reference param is mis-lowered to a value.
	void build_call_args(class TokenCallFunc *tcf, node_t args,
			     size_t param_base = 0);
	// Words of opaque storage for a runtime-object class that has a concrete ABI
	// size but no madc data members. 0 for an ordinary user class.
	size_t object_class_words(DataDefCLASS *cdd) const;
	node_t void_ptr_type();                      // N_TYPE node for a (void*) cast
	node_t char_ptr_type();                      // N_TYPE node for a (char*) cast
	node_t ptr_type_node(DataDef *dd);           // N_TYPE node for an arbitrary pointer DataDef
	node_t class_ptr_type(DataDefCLASS *cdd);    // N_TYPE node for a (struct Cls *) cast
	// Derived->base pointer/reference conversion. Returns `value` unchanged when
	// no conversion applies; otherwise emits the same base-subobject adjustment
	// recorded by class layout.
	node_t upcast_class_ptr(node_t value, DataDef *lhs_dd, class TokenBase *rhs,
				class TokenBase *origin);
	node_t upcast_class_ref_addr(node_t value, DataDefCLASS *base,
				     class TokenBase *rhs, class TokenBase *origin);
	node_t object_addr(const char *name, TokenBase *origin); // (void*)&name
	node_t object_var_void_addr(const Variable &v, TokenBase *origin);
	// Raw object-instance address of a NAMED variable (not void*-cast): the
	// pointer itself when pointer-stored (reference param, `T*`), else
	// `&name`. Shared addressing rule for every object-class receiver.
	node_t object_var_addr(const class Variable &v, TokenBase *origin);
	// Coerce an object with a c_str() method to const char* for a char*-expecting
	// call.
	node_t object_cstr_arg(TokenBase *arg);
	// Coerce an argument to an object pointer for a call whose parameter is a
	// class object (value or reference). A genuine object is passed by address;
	// any value accepted by a converting ctor is materialized into a temp.
	node_t object_arg_addr(TokenBase *arg, DataDefCLASS *target);
	// Coerce an argument to a class value for a by-value object parameter. A
	// matching object value is passed as-is; a convertible scalar/pointer is
	// materialized into a scope-local temporary first.
	node_t object_arg_value(TokenBase *arg, DataDefCLASS *target);

	// Address of an argument bound to a NON-class reference parameter
	// (`const T&`, T scalar/pointer). An lvalue passes by address directly; a
	// prvalue (a by-value call result, a post-increment, a builtin
	// arithmetic result — `_M_current++`, `it.base() - n`) is not addressable,
	// so it is materialized into a scope-lived temp whose address is passed
	// ([class.temporary]: binding a const ref to a prvalue).
	node_t ref_param_arg_addr(TokenBase *arg, DataDef *expected_referent = NULL);
	// True for the argument forms that are unambiguously prvalues and therefore
	// not addressable: a by-value-returning call, a postfix ++/--, a builtin
	// binary arithmetic/bitwise result, or a literal. Conservative by design —
	// it only flags forms that `&expr` already rejects, so lvalue arguments keep
	// the existing direct-address lowering untouched.
	bool expr_is_nonaddressable_rvalue(TokenBase *arg);

	// ---- madc array (`array`, a madc::value) object lowering ----
	// Same opaque-object model as other runtime objects; array arguments are
	// always passed by pointer and the long[] buffer name decays to that pointer
	// at the call site.
	static bool is_array_object(DataDef *dd);    // dtARRAY value type, not a pointer
	size_t array_obj_words() const;              // ceil(sizeof(madc::value)/sizeof(long))
	node_t array_storage_decl(const char *name, TokenBase *origin);
	node_t array_ctor_call(const char *name, TokenBase *origin);

	// ---- STL container (vector/map/set) object lowering ----
	// `obj[i]` on a user class defining `operator[]` -> the method call,
	// deref'd (operator[] returns T& == a T*), so it is a read/write lvalue.
	node_t class_subscript_call(class TokenSubscript *tsub, TokenBase *origin);
	// The BARE operator[] call (no deref) — for a T&-returning operator[] this
	// is the element ADDRESS (a T*). Used to take a string element's address.
	node_t class_subscript_addr(class TokenSubscript *tsub, TokenBase *origin);
	// Receiver-generic operator[] dispatch core shared by the named-variable
	// and expression-receiver subscript paths; recv_addr = receiver address.
	node_t class_subscript_addr_on(DataDefCLASS *cls, node_t recv_addr,
				       TokenBase *index, TokenBase *origin);
	// True when `arg` is `obj[i]` on a class whose operator[] yields a class
	// object element reached through the returned address.
	static bool class_subscript_is_object(TokenBase *arg);
	// True when `arg` is `base[i]` where base is a raw class pointer or fixed
	// class array. The element is reached by the address `&base[i]`.
	static bool class_array_subscript_is_object(TokenBase *arg);
	// The class an OPERAND expression denotes for operator/overload
	// resolution: its datadef's class, or — for a reference variable
	// (vfREFERENCE, stored as DataDefPTR(T)) — the referenced class. A plain
	// `T*` pointer operand stays NULL: only the reference representation is
	// transparent (pointer operands keep pointer semantics).
	DataDefCLASS *operand_object_class(TokenBase *t);
	// Wrap a type as a reference (DataDefREF), routed through the one
	// reference-creation/collapse path (Program::getReferenceType — caches +
	// collapses ref-to-ref). Used by the operator/manipulator instantiation
	// sites that synthesize FuncDefs. first-class refs Phase 4 "single collapse".
	DataDef *as_reference_type(DataDef *dd);
	// The type DOMAIN of an OPERAND's value read — the scalar twin of
	// operand_object_class: a reference variable (vfREFERENCE, stored as
	// DataDefPTR(T)) reads as its referee T; everything else is the
	// expression's datadef(). Plain pointers keep pointer semantics.
	DataDef *operand_value_datadef(TokenBase *t);

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
	// primitive, so the alias is resolved here at the cir layer). A function
	// asm-label emits the labeled symbol directly. Non-aliased variables return
	// their own name.
	std::string var_emit_name(const class Variable &v) const;
	std::string func_emit_name(const class Variable &v, class FuncDef *fd) const;
	// THE single source of truth for the C symbol a CALL references. Precedence:
	// an external ABI bind (emit_symbol, madc emits no body) wins; then a
	// madc-emitted body's non-default symbol (local_emit_name — hoisted nested
	// fn or arity-disambiguated method/operator); else the variable's own emit
	// name. Every call-symbol site routes through here so the precedence lives
	// in ONE place and cannot drift (see scripts/check-call-emit-symbol.sh).
	// The (fd, default_sym) form reads only FuncDef fields (no instance state)
	// so static helpers can delegate to it; the (Variable, fd) form supplies
	// var_emit_name(v) as the default.
	static std::string call_emit_symbol(class FuncDef *fd,
					    const std::string &default_sym);
	std::string call_emit_symbol(const class Variable &v, class FuncDef *fd) const;
	// The FuncDef behind a CALL token (direct call or fn-ptr target). THE one
	// callee resolver every consumer goes through — it substitutes a per-call
	// instantiated FuncDef (std:: free-function template bound mangled-direct
	// via emit_symbol) so arg emission, retbuf classification, and callee
	// naming all see the same instantiation.
	class FuncDef *call_target_funcdef(class TokenCallFunc *tcf);
	// std:: free-function template instantiations: one FuncDef per mangled
	// symbol, plus a per-call memo (NULL = checked, not such a call).
	std::map<class TokenCallFunc *, class FuncDef *> m_free_fn_inst_by_call;
	std::map<std::string, class FuncDef *> m_free_fn_inst_by_sym;
	std::map<class TokenOperator *, class FuncDef *> m_free_op_inst_by_call;
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
	node_t simd_vector_attrs(size_t vector_bytes, TokenBase *origin = NULL);
	// Build the type-specifier list for a compound-literal element/object type:
	// emits ID(alias) for a typedef, N_STRUCT/N_UNION for a tagged aggregate,
	// inlined members for an anonymous aggregate, else falls back to
	// append_type_specs. Shared by translate_struct_lit's scalar and array paths.
	void append_lit_type_spec(node_t spec, DataDef *dd,
				  const std::string &typedef_name);
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
			       const std::vector<carray_dim_t> &lead_dims);
	// When `fd` returns an object BY VALUE through the __retbuf ABI (void
	// return + hidden `T* __retbuf` first param), report that returned type so
	// the fn-ptr type renders the same
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
	// A parameter shape for an emitted extern prototype. `cls` (when
	// non-null) means a by-VALUE struct/union param of that class — its
	// tag is emitted via class_tag_ref and `specs`/`ptr` are ignored
	// (a by-value libstdc++ iterator/value_type arg, e.g. vector's
	// _M_fill_insert(__normal_iterator, ...)). Otherwise it's a scalar
	// (`specs`) optionally one pointer level (`ptr`). NO default member
	// initializer — that would make ExternParam a non-aggregate under
	// C++11 and break every `{ {specs}, ptr }` braced init; trailing
	// `cls` is value-initialized to null by those two-field inits.
	struct ExternParam {
		std::vector<c2mir_node_code_t> specs;
		bool ptr;
		class DataDefCLASS *cls;
	};
	// Record (once) an extern proto for an output runtime/libstdc++ symbol.
	// ret_ptr=true -> returns void*, else void. ret_specs overrides the
	// return base type when non-empty (e.g. {N_LONG} for a long-returning
	// runtime fn); empty means the default base type N_VOID.
	// `ret_cls` (when non-null) declares the return type as that class's
	// struct/union tag — for a method/function that returns a trivially-copyable
	// class BY VALUE (register-returned, no retbuf). It takes precedence over
	// ret_specs/ret_ptr (a by-value struct return is neither a scalar base nor a
	// pointer). `ret_dd` (when non-null) declares the concrete return DataDef,
	// including pointer/reference stars, so typed pointer returns such as char*
	// do not collapse to void*. Mirrors ExternParam::cls for by-value class
	// parameters.
	void need_output_extern(const char *symbol, bool ret_ptr,
				const std::vector<ExternParam> &params,
				const std::vector<c2mir_node_code_t> &ret_specs
					= std::vector<c2mir_node_code_t>(),
				class DataDefCLASS *ret_cls = NULL,
				class DataDef *ret_dd = NULL);
	void need_output_extern_unprototyped(const char *symbol, bool ret_ptr,
				const std::vector<c2mir_node_code_t> &ret_specs
					= std::vector<c2mir_node_code_t>());
	// Map a builtin print-fn name to its madc_* runtime symbol ("" if not one).
	static const char *builtin_output_runtime(const std::string &name);

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
	// The ordered member field list (vptr @0, secondary vptrs, members, and
	// explicit char padding to match compute_layout) for a class. Shared by
	// class_struct_def AND typedef_decl's inline-body path, so a class emitted
	// via a `typedef struct C {..} alias;` def-point keeps its vptr slot(s)
	// instead of falling back to the plain-struct anon_members_list (which drops
	// the vptr — breaking the inline ctor's __vptr install and the object layout).
	node_t class_member_list(DataDefCLASS *cdd);
	void emit_class_member_deps(DataDefSTRUCT *sdd, node_t top_list,
				    std::set<std::string> &emitted_structs,
				    std::set<DataDefCLASS *> &emitted_classes,
				    std::set<DataDefCLASS *> &emitting_classes);
	void emit_class_struct_with_deps(DataDefCLASS *cdd, node_t top_list,
					 std::set<std::string> &emitted_structs,
					 std::set<DataDefCLASS *> &emitted_classes,
					 std::set<DataDefCLASS *> &emitting_classes);
	// Topologically hoist a PLAIN struct/union (DataDefSTRUCT, not a class) whose
	// definition is needed before the struct that embeds it by value. Recurses
	// into its own by-value members first; emits the named struct's body (anonymous
	// aggregates are inlined at the use site, so only their member deps are hoisted).
	void emit_struct_with_deps(DataDefSTRUCT *sdd, node_t top_list,
				   std::set<std::string> &emitted_structs,
				   std::set<DataDefCLASS *> &emitted_classes,
				   std::set<DataDefCLASS *> &emitting_classes);
	// Emit a class's virtual-method dispatch table as a file-scope array of
	// type-erased function pointers in vtable_slot order:
	//   void *ClassName__vtable[] = { (void*)C__slot0, (void*)C__slot1, ... };
	// Each slot resolves to the most-derived override visible to this class
	// (findMethod walks class -> base). Returns NULL when the class has no
	// vtable. Must be emitted after the method prototypes it references.
	node_t class_vtable_def(DataDefCLASS *cdd, std::vector<node_t> &thunks);
	node_t class_typeinfo_def(DataDefCLASS *cdd); // _ZTI/_ZTS objects; NULL if non-polymorphic (S5b)
	// The vtable / typeinfo SYMBOL to reference for cdd: the madc-emitted
	// `Cls__vtable` / `_ZTI<cls>` for a class madc defines, or the REAL
	// libstdc++ `_ZTVSt.../_ZTISt...` for an externally-defined class (whose
	// machinery madc does not synthesize — see is_externally_defined()).
	std::string class_vtable_symbol(DataDefCLASS *cdd);
	std::string class_typeinfo_symbol(DataDefCLASS *cdd);
	// `extern void *SYM[];` (deduped via m_rtti_data_externs), or NULL if already
	// emitted. For referencing an externally-defined class's real _ZTVSt.../_ZTISt...
	node_t data_extern_decl(const std::string &sym);
	void vbase_ctor_stmts(const std::string &objname, bool addr_of,
			      DataDefCLASS *cdd, std::vector<node_t> &out, TokenBase *o);
	void vbase_dtor_stmts(const std::string &objname, bool addr_of,
			      DataDefCLASS *cdd, std::vector<node_t> &out, TokenBase *o);
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
	// a parsed C++ ABI symbol): declares the extern from the method's real
	// signature and routes the call directly. `this_arg` is the receiver address
	// (object_var_addr).
	node_t emit_symbol_method_call(class TokenMember *tm, class FuncDef *callee,
				       const std::string &sym, node_t this_arg,
				       TokenBase *origin);
	node_t member_template_method_call(class TokenMember *tm,
				       class FuncDef *callee,
				       node_t this_arg,
				       TokenBase *origin);
	// Build the hidden `__this` argument for a class method/operator call:
	// the receiver's address for a value receiver, or the pointer itself for
	// a pointer receiver. `recv_class` is filled with the receiver's class.
	node_t class_this_arg(class TokenMember *tm, DataDefCLASS *&recv_class,
			      TokenBase *origin);
	// Build the constructor-call statement for a class instance `v`:
	//   ClassName__ClassName(&v, ctor_args...)
	// Returns NULL when `v` is not a user class or has no user constructor.
	// When the class HAS user ctors but none matches the initializer, returns
	// an error_node (loud no-match) — never NULL, so callers cannot silently
	// drop a required construction.
	node_t class_ctor_call(class Variable *v, DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args,
			       TokenBase *origin);
	node_t class_ctor_call_addr(node_t this_addr, DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args,
			       TokenBase *origin);
	// The loud no-match result shared by both ctor-call builders: an
	// error_node naming the class and the initializer argument types.
	node_t no_ctor_match_error(DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args,
			       TokenBase *origin);
	// IMPLICIT COPY CONSTRUCTOR fallback ([class.copy.ctor]), shared by
	// both ctor-call builders' no-match tails: a same-class single argument
	// selects the implicitly-declared copy ctor; for a trivially-copyable
	// class that is a member-wise bit copy — a struct assignment into
	// `dst_lvalue`. NULL when the fallback does not apply.
	node_t try_implicit_copy_construct(node_t dst_lvalue, DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args,
			       TokenBase *origin);
	// Recursive trivial-copyability ([class.prop] subset): no own user
	// dtor, no user copy ctor, no vtable, members/bases recursively so.
	bool class_trivially_copyable(DataDefCLASS *cdd);
	bool class_trivially_copyable(DataDefCLASS *cdd,
			       std::set<DataDefCLASS *> &seen);
	// The class/type a ctor argument expression denotes for overload
	// matching (resolved callee returns, reference unwrap, array decay) —
	// shared by select_ctor_overload and try_implicit_copy_construct.
	DataDef *ctor_arg_datadef(TokenBase *arg);
	// Aggregate tag-REFERENCE node (`struct X` / `union X` per the
	// definition's union_layout) — every reference site must agree with
	// the definition's kind or c2mir rejects the tag.
	node_t class_tag_ref(DataDef *dd, TokenBase *origin = NULL);
	// Select the ctor overload of `cdd` matching the initializer arguments by
	// generic overload scoring. NULL when no overload set is recorded.
	class FuncDef *select_ctor_overload(DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args);
	// Select the operator overload (operator= / operator+=) matching the RHS by
	// generic argument scoring. Falls back to the first by-name match. NULL when
	// the class has no such operator.
	class FuncDef *select_operator_overload(DataDefCLASS *cls,
				const std::string &mname, TokenBase *rhs);
	// Lower an overloaded binary operator on a user-defined class lvalue:
	//   c <op> rhs  ->  ClassName__operator<op>(&c, rhs)
	// when c's class defines a matching operator method. Returns NULL when
	// the left operand is not a user class or has no such operator (caller
	// falls through to the built-in operator translation).
	// opsym_override substitutes the operator spelling looked up from
	// top->id() (e.g. strict equality dispatching through "=="); NULL =
	// derive from binop_overload_symbol(top->id()).
	node_t class_operator_call(class TokenOperator *top, TokenBase *origin,
				   const char *opsym_override = NULL);
	// C++20 builtin `a <=> b` ([expr.spaceship]): comparison-category temp
	// + inline byte-select into _M_value (g++ -O0 canon, no call). The
	// category class was resolved at parse time from the parsed <compare>.
	node_t three_way_builtin_lowering(class TokenOperator *top,
					  TokenBase *origin);
	// madc dialect strict equality `a === b` / `a !== b`: type-domain
	// identity AND value equality (STD_MADC only; spec
	// docs/superpowers/specs/2026-06-11-strict-equality-design.md).
	node_t strict_equality_lowering(class TokenOperator *top,
					TokenBase *origin);
	// W2 (retire-std-hardcoding-design): a NON-member operator declared at
	// namespace scope (e.g. std::operator<<(ostream&, const char*)) may be a
	// better match for `lhs <op> rhs` than the member candidate. Consider the
	// captured Program::free_operator_overloads; if a free candidate's first
	// parameter deduces-matches lhs's class and its second parameter matches rhs
	// more exactly than the member, bind it mangled-direct (itanium_mangle_std_
	// free_template) and return the call. NULL = no better free candidate (caller
	// uses the member). member_callee may be NULL (no member operator at all).
	node_t try_free_operator_call(class TokenOperator *top, DataDefCLASS *lcls,
			const std::string &mname, class FuncDef *member_callee,
			TokenBase *origin);
	// Pattern A for free namespace OPERATORS (W2 step D): overload-select +
	// template-arg deduction against the operand classes, Itanium-mangle via
	// the one mangler, and return an instantiated FuncDef carrying the
	// symbol on emit_symbol — class_operator_external_call emits it like any
	// external member operator. Handles BOTH the reference-returning stream
	// shape (operator<<(ostream&, x) -> ostream&) and the by-value class
	// return (operator+(const string&, const string&) -> string).
	// Member arbitration lives here: an exact-match member vetoes the
	// free candidate; a by-value free candidate binds only when the class
	// declares NO matching member. Memoized per operator token and per
	// symbol. NULL = the member (or generic) path keeps the call.
	class FuncDef *std_free_operator_instantiation(class TokenOperator *top,
			DataDefCLASS *lcls, const std::string &mname,
			class FuncDef *member_callee);
	// Emit `lhs <op> rhs` against a callee bound to an EXTERNAL ABI symbol
	// (FuncDef::emit_symbol): lhs by address (parameters[0]'s class when it
	// names one — a free operator's BASE param binds a derived lhs via
	// object_arg_addr's base walk — else the lhs class), rhs shaped by
	// parameters[1]. A reference return is dereferenced to the lvalue; a
	// NON-TRIVIAL by-value class return uses the Itanium sret/__retbuf
	// shape — into ret_slot when the caller provides one (declaration-init
	// copy elision; *slot_used reports it, caller wraps the bare call as a
	// statement), else into a cleanup-tagged temp whose object lvalue is
	// the expression value.
	node_t class_operator_external_call(class TokenOperator *top,
			DataDefCLASS *lcls, class FuncDef *callee, TokenBase *origin,
			node_t ret_slot = NULL, DataDefCLASS *slot_cls = NULL,
			bool *slot_used = NULL);
	// Instantiate a NAMED std:: free-function template for a call (overload
	// select + template-arg deduction from the args' classes + Itanium mangle)
	// and return a FuncDef carrying the symbol on emit_symbol — the generic
	// call path does the rest (Pattern A). Memoized per call token and per
	// symbol. NULL = not such a call (caller keeps the declared FuncDef).
	class FuncDef *std_free_function_instantiation(class TokenCallFunc *tcf,
			class FuncDef *cdf);
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
	// Append construct / destruct calls for every embedded OBJECT member of
	// `cdd` to `out`, addressing each member through the in-scope `__this`
	// pointer. The called symbols come from the member class registration.
	// Used to give a class with object members proper member lifetime inside
	// its (possibly synthesized) ctor/dtor. Returns true if it emitted any.
	bool class_member_construct(DataDefCLASS *cdd, std::vector<node_t> &out,
				    TokenBase *origin,
				    const std::set<std::string> *skip = NULL);
	bool class_ctor_initializer_stmts(DataDefCLASS *cdd, FuncDef *fd,
				    std::vector<node_t> &out, TokenBase *origin);
	// Apply C++11 default member initializers (NSDMI: `int x = 5;`) for any
	// scalar/pointer member not explicitly initialized (not in `skip`). The
	// receiver is `recv`, accessed `recv->member` when `arrow` (a ctor body's
	// `__this`) or `recv.member` otherwise (a named local). Object members are
	// value-initialized by the existing member-construction path, not here.
	bool emit_member_default_inits(DataDefCLASS *cdd, const char *recv,
				    bool arrow, std::vector<node_t> &out,
				    TokenBase *origin,
				    const std::set<std::string> *skip = NULL);
	bool class_member_destruct(DataDefCLASS *cdd, std::vector<node_t> &out,
				   TokenBase *origin);
	// True when the class has at least one embedded object member needing
	// construction/destruction (so it requires a ctor/dtor even if the user
	// wrote none).
	bool class_has_object_members(DataDefCLASS *cdd);
	// Append constructor statements (one per embedded object member) to the
	// c2mir list node `items`. Used at a
	// value class-instance declaration that has object members but no user
	// constructor (the member access is `inst.member`, not `__this->member`).
	void class_instance_member_ctors(const char *inst, DataDefCLASS *cdd,
					 node_t items, TokenBase *origin);
	// True when a class needs an (implicit) destructor: it has a user dtor,
	// embedded object members, or a base class that needs one.
	bool class_needs_dtor(DataDefCLASS *cdd);
	// The class's OWN destructor method (the one it wrote itself), or NULL.
	// Found name-independently: the method_map carries a "~"-prefixed key for
	// every reachable dtor (own + inherited via the base-merge at parser.cpp
	// ~20600), but ONLY own methods are in cdd->methods — inherited entries are
	// copied into method_map alone. So the own dtor is the "~"-keyed entry whose
	// Variable is also in cdd->methods. Keyed on the source tag ("~Inner"), NOT
	// on cdd->name, so it survives a nested/instantiated class whose composed
	// name (Outer_int32_t__Inner) differs from its tag (Inner).
	Variable *class_own_dtor(DataDefCLASS *cdd);
	// True only when THIS class wrote its own ~Cls(). has_user_dtor is inherited
	// (a derived class with a non-trivial base also sets it), so it cannot gate
	// synthesis: a class with a non-trivial base but no OWN dtor still needs a
	// synthesized Cls___dtor chaining to the base.
	bool class_has_own_user_dtor(DataDefCLASS *cdd);
	// The destructor symbol used as the cleanup function for a class
	// instance (ClassName___dtor) — whether user-written or synthesized.
	std::string class_dtor_symbol(DataDefCLASS *cdd);
	std::string class_complete_dtor_symbol(DataDefCLASS *cdd);
	node_t synth_complete_dtor_def(DataDefCLASS *cdd);
	node_t synth_deleting_dtor_def(DataDefCLASS *cdd);
	node_t synth_dtor_proto(const std::string &sym, DataDefCLASS *cdd);
	// Emit a synthesized destructor function for a class that needs a dtor
	// (object members and/or a base dtor) but has no user-written one.
	// Returns NULL when the class has a user dtor (its own def handles this)
	// or needs no dtor.
	node_t synth_dtor_def(DataDefCLASS *cdd);
	node_t func_proto(TokenFunc *tf);
	node_t func_def(TokenFunc *tf);
	// Scan file-scope class-instance globals that are not source-declared via
	// top_decls (built-ins like `version`, registered
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
	// Translate a single (possibly non-compound) controlled statement — an
	// if/else branch or a loop body — scoping any temporaries it materializes
	// into a wrapping block so they are declared before the statement and
	// cleaned up at its exit (a compound statement already does this via
	// translate_block; a bare statement has no scope of its own).
	node_t translate_branch_stmt(TokenBase *tb);
	// A loop body's own temporaries must live INSIDE the body so they are
	// re-constructed each iteration; wraps a non-compound body (reusing
	// translate_branch_stmt) but stashes the loop's init/cond/incr pending temps
	// first so only the body's temps are wrapped.
	node_t translate_loop_body(TokenBase *tb);
	node_t translate_block(TokenCpnd *tc);
	node_t translate_return(TokenRETURN *tr);
	node_t translate_if(TokenIF *ti);
	node_t translate_if_core(TokenIF *ti);
	node_t translate_while(TokenBase *tw);
	node_t translate_for(TokenFOR *tf);
	// SJLJ exception lowering: try/catch -> setjmp on __madc_try_push(&ctx), the
	// try body on the ==0 arm (then __madc_try_pop), the catch dispatch on the
	// else arm (by __madc_exception_type, bind value, run handler, clear; no
	// match -> __madc_rethrow). Emitted as block-scoped statements (NOT a stmt-
	// expr — see the c2mir cleanup-scope gotcha). throw -> __madc_throw_*.
	node_t translate_try(class TokenTRY *tt);
	node_t translate_throw(class TokenTHROW *th);
	node_t translate_throw_call(class TokenTHROW *th);
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
	// Range-based for over a madc array (madc::value): `for (T x : arr) body`. The loop
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
	// subscript `a[__i]`. (No madc-array runtime helper.)
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
