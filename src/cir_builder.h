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
#include <unordered_set>
#include <cassert>
#include <map>
#include <vector>
#include <functional>

// Forward declarations
class Program;
struct memberpair_t;
class TokenFunc;
class TokenCpnd;
class TokenBase;
class TokenPackExpansion;
class TokenIF;
class TokenFOR;
class TokenDO;
class TokenSWITCH;
class TokenCASE;
class TokenRETURN;
class TokenOperator;
class Variable;
class DataDef;
class DataDefTemplateParam;
class DataDefSTRUCT;
class DataDefCOMPLEX;
class FuncDef;
class Method;

// Membership set of ODR-used function symbols with speculative-translation
// journaling. referenced_funcs is membership-only (insert/count — never
// iterated, never erased), so the container is unordered; the speculative
// translation paths (tsubst pattern probes, deferred-construction re-lowers)
// used to save/restore it by FULL SET COPY — three tree clones per attempt.
// mark()/rollback() undo exactly the keys inserted since the mark instead
// (only first-time inserts journal, so rollback restores the precise prior
// membership). Scope is the RAII form: dtor COMMITS (keeps inserts, pops the
// mark) when rollback() wasn't called — matching the old copy pattern's
// behavior on exception unwind, where the restore line never ran.
class RefFuncSet {
	std::unordered_set<std::string> s_;
	std::vector<std::string> journal_;	// keys newly inserted while marked
	std::vector<size_t> marks_;
public:
	void insert(const std::string &k)
	{
		if (s_.insert(k).second && !marks_.empty())
			journal_.push_back(k);
	}
	size_t count(const std::string &k) const { return s_.count(k); }
	void mark() { marks_.push_back(journal_.size()); }
	void rollback()
	{
		size_t m = marks_.back();
		marks_.pop_back();
		for (size_t i = journal_.size(); i-- > m; )
			s_.erase(journal_[i]);
		journal_.resize(m);
	}
	void commit()
	{
		marks_.pop_back();
		if (marks_.empty())
			journal_.clear();
	}
	size_t depth() const { return marks_.size(); }
	class Scope {
		RefFuncSet &r_;
		size_t depth_;	// mark-stack depth OWNED by this scope
		bool done_;
	public:
		explicit Scope(RefFuncSet &r)
			: r_(r), depth_(r.depth() + 1), done_(false) { r_.mark(); }
		// Must run at this scope's own depth — a rollback while an inner
		// mark is still live would pop the wrong journal boundary.
		void rollback() { assert(r_.depth() == depth_); r_.rollback(); done_ = true; }
		~Scope() { if (!done_) { assert(r_.depth() == depth_); r_.commit(); } }
	};
};

class CirBuilder {
	c2m_ctx_t c2m;
	CirArena arena;
	// Function names referenced (called) while translating bodies. Used by
	// translate_module to emit extern prototypes only for funcs the source
	// actually uses (matches c2m's #include-driven scope).
	RefFuncSet referenced_funcs;
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
	// TU identity (the source path) — set by the caller before
	// translate_module; seeds the object-mode per-TU init symbol.
	std::string m_tu_name;
	// Object mode (ELF-completion S3): the TU-unique STATIC init function
	// translate_module synthesized (empty = this TU has none). madc_cir
	// registers it into the capture's .init_array after generation.
	std::string m_tu_init_name;
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
	// Rung 3 (referenced-only surface): per-global ctor statement groups —
	// which m_global_ctor_stmts entries belong to which file-scope global —
	// so the filter can drop a dead system-header global's ctor calls from
	// __madc_global_init together with its storage decl. Parallel to the
	// order collect_global_ctors appends: one entry per global, holding its
	// Variable and that global's slice of statements.
	std::vector<std::pair<Variable *, std::vector<node_t> > > m_ctor_groups;
	// Rung 3: the Pass-0/collect-time storage decl node of each file-scope
	// global (keyed by Variable) — links a ctor group to its decl node.
	std::map<Variable *, node_t> m_global_decl_node;
	// C++ dynamic initialization (g++ model): file-scope globals whose
	// scalar initializer is NOT a C11 constant expression (it reads a
	// variable or calls a function). var_decl emits their storage without
	// the initializer; collect_global_ctors queues the full source
	// assignment into __madc_global_init, in declaration order.
	std::set<Variable *> m_dynamic_global_inits;
	// True while var_decl emits a FILE-SCOPE declaration (the dkGlobalVar
	// pass) — the only context where the dynamic-init routing applies;
	// block-scope declarations take runtime initializers natively.
	bool m_file_scope_decl = false;
	// Wide string literals (parser addWideLiteral): the sanitized module
	// symbol (__wlit_<n>) each synthetic __wliteral__ Variable emits under.
	// The Variable's own name embeds the raw UTF-32 payload (binary-safe for
	// parse-time dedup, NOT a valid C identifier). Populated by the
	// translate_module pre-scan that also emits the definitions; read by
	// var_emit_name. Cleared per module.
	std::map<const Variable *, std::string> m_wide_literal_syms;
	// Rung 3: the conditional-emission map. A node recorded here survives the
	// end-of-translate referenced-surface filter only if referenced: TYPE
	// nodes (is_type) by struct tag — or, for typedef-bearing decls, any of
	// their identifiers — through admitted decls' own references (fixpoint);
	// SYMBOL nodes by their declared name (key). Everything NOT in this map
	// is a root and seeds the reference harvest. Populated at the emission
	// sites (only for system-header-origin entities), cleared per module.
	struct CondEmit { bool is_type; std::string key; };
	std::map<node_t, CondEmit> m_cond_nodes;
	void cond_mark_type(node_t n) {
		if (n) { CondEmit c; c.is_type = true; m_cond_nodes[n] = c; }
	}
	void cond_mark_sym(node_t n, const std::string &key) {
		if (n && !key.empty()) {
			CondEmit c; c.is_type = false; c.key = key;
			m_cond_nodes[n] = c;
		}
	}
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

	// ---- Pack-time drain / check-gate state (rung 1) ----
	// Hoisted from translate_module locals so the pack-side c2mir check gate
	// (pack_gate_drop, driven by madc_cir_freeze AFTER translate returns) can
	// drop check-defective drained defs, revert them to DEFBODY, and re-run
	// the callee cascade post-hoc. All cleared at translate_module entry;
	// meaningful only under prog->pack_recording.
	std::set<std::string> drain_failed_syms;
	std::map<std::string, Program::DeferredFunctionBody> drain_saved;
	std::vector<std::pair<TokenFunc *, node_t> > pack_defs;
	std::vector<std::set<std::string> > pack_def_callees;
	std::vector<char> pack_is_dropped;
	std::set<std::string> pack_dropped;
	// Symbols whose tree-resident defs must NOT stamp DF_HAS_FOREST_BODY
	// (consumer-excluded under the emission split): DEFBODY-reverted bodies
	// and cascade-excluded callers. Consumed by
	// madc_cir_freeze (erased from funcdef_locs pre-arena_complete).
	std::set<std::string> pack_stamp_excluded;
	// Symbol -> the Pass-1.95 forward-proto node of a materialized body.
	// When the check gate drops a def, its proto must leave the tree with
	// it — a defective def's proto can carry the def's broken ABI shape and
	// conflict with the Pass-0.75 extern ("incompatible declarations").
	std::map<std::string, node_t> pack_proto_nodes;
	std::set<std::string> user_func_names;
	std::map<std::string, size_t> pack_stash_idx;
	std::set<std::string> pack_synth_dtor_syms;
	std::map<std::string, bool> pack_dlsym_memo;
	// Drop a stashed pack def: revert its DEFBODY (drain_saved) so consumers
	// derive the body on use, else mark the symbol un-carriable so callers
	// cascade-drop. Every drop is logged (no silent caps).
	void pack_record_drop(TokenFunc *tf, const char *why, bool always_unsafe);
	// TRUE when a bound consumer can resolve sym: a surviving sibling stash,
	// a (non-local-class) DEFBODY derived on use, a TU-root user fn, a synth
	// dtor, a c2mir builtin, a forest-carried body, or a dlsym external.
	bool pack_callee_homed(const std::string &sym);
	// Drop every stashed def calling an un-homed symbol, to fixpoint.
	void pack_run_cascade();

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
	node_t ref_param_arg_addr(TokenBase *arg,
				  DataDef *expected_referent = NULL,
				  bool allow_converted_temp = false);
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
	// Unwind a subscript token tree (TokenSubscript / TokenSubscriptExpr
	// chain) to its named root variable + index list in the linearizer's
	// order; false when tb is not a pure subscript tree over a named root.
	bool subscript_root_indices(TokenBase *tb, class Variable *&root,
				    std::vector<TokenBase *> &idxs);
	// Row stride (element count) for pointer arithmetic on a flat-lowered
	// runtime-sized array pointer value; NULL when no scaling applies.
	node_t vla_arith_stride(TokenBase *t, TokenBase *origin);
	// Linearized access on a flat VLA pointer (runtime-sized param or
	// malloc'd local): full index chains yield the element lvalue, partial
	// chains yield C's row pointer as scaled pointer arithmetic. idxs is
	// outermost-first; NULL when root is not a flat runtime-sized chain.
	node_t vla_flat_subscript(class Variable *root,
				  const std::vector<TokenBase *> &idxs,
				  TokenBase *origin);
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

	// Two-tree Phase 3: memoized Tree-1 body cir pattern per SOURCE member
	// template — built ONCE from FuncDef::dependent_pattern (the recipe),
	// copied+substituted into a fresh Tree-2 body per instantiation. Keyed by
	// the source template FuncDef. The pattern lives in this builder's arena
	// (immutable Tree-1), never freed mid-compile.
	std::map<class FuncDef *, cir_node *> m_tsubst_body_patterns;
	// Phase-5 slice 1: memoized Tree-1 mem-initializer pattern per SOURCE
	// ctor template — each ci's ARG expressions lowered ONCE in pattern
	// mode alongside the body pattern; copied+substituted per instantiation
	// and emitted ahead of the tsubst body (whole-ctor switch: func_def's
	// prologue skips its shell-token-side emission when the hit body
	// already carries the mem-inits). Only ASSIGN-path members (scalar /
	// pointer / reference — no class-typed member ctor-call, no base
	// inits) are admitted for now; anything else keeps the shell
	// path (absent map entry). A DELEGATING ci ([class.base.init]p6: the
	// sole initializer, and the target ctor performs the COMPLETE
	// initialization) keeps its TOKEN args instead: the target is
	// overload-SELECTED per instantiation (arity/types vary with the
	// pack), so the hit path relowers the whole ctor call.
	struct TsubstMemInitPattern {
		std::string name;		// ci name (member, per fd source order)
		cir_node *arg = NULL;		// the single lowered arg expr (NULL = `member()` value-init)
		bool value_init = false;	// `member()` — zero-init scalar/pointer
		bool delegating = false;	// delegation: relower the target ctor call at hit
		bool construct = false;		// member CONSTRUCTION ci (class member /
						// pack-expansion args): relowered at hit
		std::vector<TokenBase *> ci_args; // token args for the relower shapes
	};
	std::map<class FuncDef *, std::vector<TsubstMemInitPattern> > m_tsubst_meminit_patterns;
	// Set by a tsubst_method_body HIT whose returned body already carries the
	// substituted mem-init statements; func_def reads+clears it to suppress
	// the shell-side ctor-init emission for exactly that ctor.
	bool m_tsubst_body_carries_meminits = false;
	// Phase-5 slice 3: set when tsubst_method_body bailed on a COVERED shape
	// (pattern built + binding complete) and returned a LOUD error body
	// instead of NULL — func_def counts it in the fallback profile (not as a
	// hit) and must NOT re-parse; the error node aborts the compile at the
	// pre-c2mir gate. Reset at every tsubst_method_body entry.
	bool m_tsubst_bailed_covered = false;
	// True while cir-building a Tree-1 recipe pattern: a template-parameter
	// placeholder reaching type lowering is left as a deferred type-spec MARKER
	// (the g++ TEMPLATE_TYPE_PARM-in-the-saved-tree model) instead of erroring,
	// so tsubst can expand it to the concrete type per instantiation. False
	// everywhere else (a stray placeholder at type lowering stays a hard error).
	bool m_tsubst_pattern_mode = false;
	// Active during tsubst body copy when the concrete member-template instance
	// carries type argument packs. Keys are template parameter indices; values
	// are the DataDefTemplateParam placeholders used in the saved Tree-1 recipe.
	const std::vector<std::vector<DataDef *> > *m_tsubst_active_type_arg_packs = NULL;
	std::map<unsigned, DataDef *> m_tsubst_active_pack_params;
	// subst_datadef under THIS builder's active pack window — the seam every
	// CirBuilder-side type substitution goes through, so the structural
	// dependent-shell rebuild (which needs pack arity for sizeof.../
	// __integer_pack forms) is available uniformly. Outside a tsubst window
	// the pack fields are empty and this is plain subst_datadef.
	DataDef *subst_datadef_active(DataDef *dd,
				      const std::map<DataDef *, DataDef *> &subst);
	// Bind EVERY pack mentioned in an expansion pattern (incl. non-type
	// index packs in nested calls' explicit template args) to its elem-th
	// window element — lockstep expansion per [temp.variadic]. False on a
	// lockstep arity violation.
	bool tsubst_bind_lockstep_packs(TokenBase *pattern, size_t elem,
					std::map<DataDef *, DataDef *> &elem_subst);
	int m_tsubst_copy_pack_index = -1;
	size_t m_tsubst_copy_pack_elem = 0;
	uint32_t m_tsubst_copy_pack_value_id = 0;	// strpool handle (0 = none)
	bool m_tsubst_copy_under_deref = false;
	// Active during a tsubst body copy: maps a Tree-1 pattern LOCAL-class method's
	// emit symbol -> the concrete instantiation's corresponding method emit symbol
	// (e.g. `_M_construct`'s `_Guard` ctor/dtor). A local class defined in a
	// template body is instantiated WITH its enclosing method (g++ TAG_DEFN), so
	// the parser already built the concrete `<owner>__<local>` class + its methods;
	// the pattern body's raw-copied ctor/dtor calls are retargeted to those. Empty
	// outside a tsubst copy.
	std::map<std::string, std::string> m_tsubst_local_method_remap;
	// Build a concrete instantiated member-template method's BODY by tsubst of
	// its source template's Tree-1 recipe (instead of lowering the re-parsed
	// body — hybrid B keeps the concrete signature/shell on the parse path).
	// Returns NULL when this is not a covered method, so the caller falls back
	// to translate_block; capability-gated per body.
	node_t tsubst_method_body(class TokenFunc *tf, class FuncDef *fd,
				  const char **reason_out = NULL);
	// Memo of the emittable-symbol sets tsubst_method_body's callee gate
	// consults (Pass-1.6 synth dtor symbols; forest-loaded body symbols).
	// INCREMENTAL: each struct_map/funcdef_map entry is classified ONCE
	// (tracked by pointer in the seen sets) because its verdict is stable
	// during translation — registrations arrive COMPLETE (the builder-side
	// struct_map insert finalizes layout first; MTI products register
	// whole) and has_forest_body is stamped at restore, pre-translate. A
	// size stamp is NOT enough here: struct_map grows between most tsubst
	// calls (MTI class derivations), which would force a full ~1k-string
	// rebuild per call — the 500M-Ir flood this memo exists to kill. A
	// same-name re-registration keeps the old entry's symbols; that is
	// name-derived (class_dtor_symbol), so the symbol survives replacement
	// and a mismatch would fail LOUD at MIR link, never silently.
	std::set<std::string> m_synth_dtor_syms_memo;
	std::set<std::string> m_forest_body_syms_memo;
	std::unordered_set<const void *> m_emit_sets_seen_classes;
	size_t m_emit_sets_struct_count = 0;	// struct_map never erases: equal size = no walk
	// funcdef_map's forest-body subset is restore-stamped (fixed before
	// translation) and its nodes CAN be erased mid-translate, so it gets
	// one walk per module instead of node-address seen tracking.
	bool m_emit_sets_funcdefs_done = false;

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
	// Record a file-scope reference so pass 0.78 emits its extern decl.
	// THE one owner: every path that reads a global BY NAME must call it,
	// or the emitted C references an identifier c2mir never saw.
	void note_global_reference(const class Variable &v);
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
	class Variable *call_target_variable(class TokenCallFunc *tcf,
					     class FuncDef **fd_out = NULL);
	class FuncDef *call_target_funcdef(class TokenCallFunc *tcf);
	std::string call_target_emit_name(class TokenCallFunc *tcf,
					  class FuncDef **fd_out = NULL);
	// std:: free-function template instantiations: one FuncDef per mangled
	// symbol, plus a per-call memo (NULL = checked, not such a call).
	std::map<class TokenCallFunc *, class FuncDef *> m_free_fn_inst_by_call;
	std::map<std::string, class FuncDef *> m_free_fn_inst_by_sym;
	std::map<class TokenOperator *, class FuncDef *> m_free_op_inst_by_call;
	std::map<class TokenOperator *, class Variable *> m_free_op_body_by_call;
	node_t integer(long val, TokenBase *origin = NULL);
	// Type-aware integer literal: pick the c2mir literal node code
	// (N_I/N_U/N_L/N_UL) from the literal's own DataDef so a suffixed
	// constant (e.g. `0xffffffffull`) carries its real signedness/width
	// into c2mir's usual-arithmetic-conversion logic. A >64-bit value
	// (P0 slice 3) has no C literal form — it lowers to the composed
	// ((unsigned __int128)hi << 64) | lo expression (Tier-1; c2mir folds
	// it back to one 128-bit constant at check time).
	node_t integer_typed(madc_wide_int val, DataDef *dd, TokenBase *origin = NULL);
	node_t real(double val, TokenBase *origin = NULL);
	node_t real_float(float val, TokenBase *origin = NULL);
	node_t real_ldouble(long double val, TokenBase *origin = NULL);
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
	// ---- GNU integer-_Complex lowering (struct spine) ----
	// c2mir carries no integer complex (and rejects the specifier), so every
	// integer-element DataDefCOMPLEX value/op lowers componentwise here,
	// with gcc's tree-complex.cc semantics (Smith division in integer
	// arithmetic). as_lowered_complex() (cir_builder.cpp) gates every hook;
	// floating-element complex stays on c2mir's native path.
	// Tag reference + struct_map registration for the late-struct sweep
	// (Pass 1.97) — the #68 use_builtin_va_list pattern.
	node_t int_complex_struct_ref(DataDefCOMPLEX *cdd);
	// (struct C){re, im} compound literal — re/im nodes are adopted.
	node_t int_complex_compound(node_t re, node_t im, DataDefCOMPLEX *cdd,
				    TokenBase *origin);
	// Declare a block-local temp of dd into `items`; returns its name.
	std::string int_complex_temp(node_t items, DataDef *dd, TokenBase *origin);
	// Convert an already-translated value of src_dd to the lowered type
	// `to` (componentwise; scalar -> {v,0}; native -> {creal,cimag}).
	node_t int_complex_from_node(node_t val, DataDef *src_dd,
				     DataDefCOMPLEX *to, TokenBase *origin);
	// Translate a token and convert it to the lowered type `to`.
	node_t int_complex_value(TokenBase *src, DataDefCOMPLEX *to,
				 TokenBase *origin);
	// Lowered -> native complex: (re + im * 1.0i), imaginary-unit width
	// picked from native_dd. Lowered -> scalar: the real component.
	node_t int_complex_to_native(node_t val, DataDefCOMPLEX *from,
				     DataDef *native_dd, TokenBase *origin);
	node_t int_complex_to_scalar(node_t val, DataDefCOMPLEX *from,
				    TokenBase *origin);
	// Componentwise statements for `<ar,ai> op <br,bi>` into temp `tname`
	// (component builders are closures — c2mir nodes hold ONE parent link,
	// so every use site must construct a fresh tree).
	void int_complex_emit_op(node_t items, TokenID op, DataDefCOMPLEX *C,
				 const std::function<node_t()> &ar,
				 const std::function<node_t()> &ai,
				 const std::function<node_t()> &br,
				 const std::function<node_t()> &bi,
				 const std::string &tname, TokenBase *tb);
	// Binary/unary interception from translate_expr's operator arms.
	// Return NULL when not applicable (caller falls through).
	node_t int_complex_binop(TokenOperator *top, TokenBase *tb);
	node_t int_complex_unary(TokenOperator *top, TokenBase *tb);
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
	bool fnptr_alias_is_fn(const std::string &alias);

	// ---- Declaration builders ----
	// Recursively build an initializer value node: a scalar expression, or
	// for a nested brace element (TokenStructLit) a LIST(INIT(LIST(), val), ...).
	node_t init_value(TokenBase *elem, bool target_is_aggregate = false);
	// True when positional slot `idx` of a brace initializer for aggregate
	// type `dd` targets a member/element that is itself an aggregate (a
	// fixed array or nested struct/union) — used so a designated-init GAP on
	// that slot emits a nested zero-init `{}` instead of a scalar 0.
	bool init_slot_is_aggregate(DataDef *dd, size_t idx);
	// The declared type behind a positional initializer slot (array element
	// / struct member); NULL when unknown.
	DataDef *init_slot_type(DataDef *dd, size_t idx);
	// Compile-time (re,im) fold of an integer-complex constant expression,
	// and the {re, im} brace list it emits into a static initializer.
	bool int_complex_const_fold(TokenBase *tb, long &re, long &im);
	node_t int_complex_init_list(long re, long im, TokenBase *origin);
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
	// v20 (forest bind): declare the extern for a COMPILER-RUNTIME symbol a
	// LOADED forest body references (__madc_* exception/cleanup runtime,
	// setjmp, malloc/calloc/free, __madc_vla_free) with the SAME signature
	// the live lowering site declares — a loaded body is the producer's
	// lowered output, so its runtime calls arrive pre-built and never pass
	// through the lowering site that would have declared them. Returns true
	// when `sym` is such a runtime symbol. This is the compiler's OWN fixed
	// runtime ABI set (the extern-C compiler-machinery category), not a
	// user-name special case.
	bool ensure_runtime_extern_for(const std::string &sym);
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
	// _ZTI/_ZTS objects; NULL for a vptr-less class unless forced — a
	// vptr-less BASE referenced by an emitted class's typeinfo is forced
	// (recursively, deduped via m_forced_base_typeinfos), else its _ZTI
	// stays an undefined import. (S5b; task #49)
	node_t class_typeinfo_def(DataDefCLASS *cdd, bool force = false);
	std::set<DataDefCLASS *> m_forced_base_typeinfos;
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
	// Core of vbase_ctor_stmts with a minted receiver address (a fresh node
	// per vbase — array elements / heap temps have no bare object name).
	void vbase_ctor_stmts_addr(const std::function<node_t()> &mint_addr,
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
	// Complete-object (Itanium C1-flavor) construction at a minted address:
	// user-ctor virtual bases first (base-most order), then the C2-flavor
	// construction (class_ctor_call_addr). `mint_addr` returns a FRESH typed
	// address node per call (c2mir single parent link). Statements append
	// to `out`. The one assembler behind heap and array-element sites.
	void complete_object_construct_stmts(
			       const std::function<node_t()> &mint_addr,
			       DataDefCLASS *cdd,
			       const std::vector<TokenBase *> &ctor_args,
			       TokenBase *origin, std::vector<node_t> &out);
	// Per-element complete-object construction loop over a class array:
	// `for (long __ci = 0; __ci < <count>; __ci += 1)
	//      <construct (arr_ptr + __ci)>;`
	// NULL when the element class needs no construction (trivial).
	node_t class_array_construct_loop(const char *arr_ptr,
			       const std::function<node_t()> &mint_count,
			       DataDefCLASS *cdd, TokenBase *origin);
	// Itanium new[] cookie size: max(sizeof(size_t), alignof) when the
	// element class has a non-trivial dtor (delete[] reads the element
	// count back to run per-element dtors), else 0 — new[] and delete[]
	// must agree, so both call this.
	size_t class_array_cookie_size(DataDefCLASS *cdd);
	// True when ctorless `cdd`'s implicit default ctor must construct base
	// subobjects: some transitive NON-VIRTUAL base has a callable user
	// default ctor (virtual bases are the complete-object site's duty).
	bool class_needs_base_construction(DataDefCLASS *cdd);
	// The pure-virtual slot (if any) that makes `cdd` abstract — the slot
	// name whose most-derived resolution is still `= 0`; "" when concrete.
	std::string class_pure_virtual_of(DataDefCLASS *cdd);
	// Dispatch a destructor through the receiver's vtable dtor slot; sname
	// is "~" (D1 complete — explicit p->~X()) or "~$deleting" (D0 —
	// delete). recv_vptr/recv_arg = two independent receiver translations.
	node_t virtual_dtor_slot_call(DataDefCLASS *cdd, const char *sname,
			       node_t recv_vptr, node_t recv_arg,
			       TokenBase *tb);
	// Default-construct the non-virtual base subobjects of ctorless `cdd`
	// through the named receiver pointer, recursing through ctorless
	// layers with the accumulated offset.
	void append_base_default_constructs(node_t items, const char *recv_ptr,
			       DataDefCLASS *cdd, size_t off0,
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
	// Memberwise reconstruction walk for the implicit copy ctor's
	// NON-trivial arm (task #70): after the whole-object bit-copy,
	// re-invoke the USER copy ctor of every (possibly nested) class
	// member that declares one — `lname->path` from `rname->path`.
	// Copy-ctor-less non-trivial members recurse; scalar bytes are
	// already correct from the bit-copy.
	void implicit_copy_member_reconstructs(DataDefCLASS *cdd,
			       const char *lname, const char *rname,
			       std::vector<std::string> &path,
			       std::vector<node_t> &out, TokenBase *origin);
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
	// `done_bases` = indices of BASE subobjects whose ctor/dtor this
	// function already emitted. Members flattened in from those bases are
	// that base's lifetime, not ours — see member_origin in datadef.h.
	bool class_member_construct(DataDefCLASS *cdd, std::vector<node_t> &out,
				    TokenBase *origin,
				    const std::set<std::string> *skip = NULL,
				    const std::set<int> *done_bases = NULL);
	// `sym((void*)recv->member)` expression statement — madarray_construct /
	// madarray_destruct on a madc `array` (madc::value) data member.
	node_t array_member_runtime_call(const char *sym, bool returns_value,
					 const char *recv_ptr,
					 const std::string &mname,
					 TokenBase *origin);
	// madc `array` element READ (`arr[i]`): string temp filled from
	// __php_array_get_cstr (string typing) or __php_array_get_int value.
	node_t madc_array_subscript_read(node_t container_void, node_t index_node,
					 DataDefCLASS *scls, TokenBase *origin);
	bool class_ctor_initializer_stmts(DataDefCLASS *cdd, FuncDef *fd,
				    std::vector<node_t> &out, TokenBase *origin);
	// Aggregate list-initialization of a member ([dcl.init.aggr]):
	// `Foo() : p{1,2}` assigns the flattened argument sequence to the
	// member's FIELDS in declaration order, recursing into nested
	// aggregates. `path` is the field chain from `__this`; the access
	// expression is rebuilt per statement so no c2mir node is shared
	// between two parents.
	bool aggregate_member_init_stmts(std::vector<std::string> &path,
				    DataDef *mtype,
				    const std::vector<TokenBase *> &args,
				    size_t &ai, std::vector<node_t> &out,
				    TokenBase *origin);
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
				   TokenBase *origin,
				   const std::set<int> *done_bases = NULL);
	// True when the class has at least one embedded object member needing
	// construction/destruction (so it requires a ctor/dtor even if the user
	// wrote none).
	bool class_has_object_members(DataDefCLASS *cdd);
	// Default-construct every class-type member of `cdd` through the NAMED
	// pointer variable `recv_ptr`, appending to the c2mir list node
	// `items` (a fresh id() per member — c2mir nodes hold a single parent
	// link, so a receiver node cannot be shared). Returns true when
	// anything was emitted. The one member loop behind implicit default
	// construction (class_ctor_call_addr's ctorless arm, the ctorless
	// `new` path).
	bool append_member_default_constructs(node_t items,
					      const char *recv_ptr,
					      DataDefCLASS *cdd,
					      TokenBase *origin);
	// True when ctorless `cdd`'s implicit default construction must emit
	// member statements (some member has a callable default ctor or is a
	// ctorless class that itself needs construction).
	bool class_needs_member_construction(DataDefCLASS *cdd);
	// Owner-subobject adjust through a VIRTUAL base, read from the
	// vtable's vbase-offset slot at runtime (Itanium): a receiver whose
	// STATIC class is not the object's most-derived type cannot use the
	// static base_offset_of. Emits a stmt-expr consuming `this_arg`
	// exactly once. NULL when not applicable (caller keeps the static
	// adjust). Slice 1: externally-defined view classes only (real
	// libstdc++ vtables carry the slots; madc-emitted ones do not yet).
	node_t vbase_dynamic_adjust(node_t this_arg, DataDefCLASS *view,
				    DataDefCLASS *owner, TokenBase *origin);
	// True when a class needs an (implicit) destructor — the g++
	// [class.dtor] TRANSITIVE test: a user dtor, an embedded object member
	// whose class itself needs one, or a base that needs one. A class whose
	// members are all trivially destructible is trivially destructible: no
	// synth dtor is emitted and no cleanup attribute references one (gcc
	// emits nothing for such classes either). Memoized per builder.
	bool class_needs_dtor(DataDefCLASS *cdd);
	std::map<DataDefCLASS *, bool> m_needs_dtor_memo;
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
	// True iff Pass 1.6 synthesizes a base dtor (Cls___dtor) for this class.
	bool class_gets_synth_dtor(DataDefCLASS *cdd);
	// The destructor symbol used as the cleanup function for a class
	// instance (ClassName___dtor) — whether user-written or synthesized.
	std::string class_dtor_symbol(DataDefCLASS *cdd);
	std::string class_complete_dtor_symbol(DataDefCLASS *cdd);
	// Per-(class,N) stack-array destructor wrapper `Cls__arr<N>___dtor`: the
	// cleanup attribute calls ONE function with &arr, so a fixed array of a
	// dtor-carrying class destroys its N elements in REVERSE through this
	// wrapper (g++ [class.dtor] order; mirrors the delete[] cookie arm).
	std::string class_array_dtor_symbol(DataDefCLASS *cdd, size_t n);
	// Demand the wrapper: synthesize+record its definition once (emitted with
	// the Pass 1.95 late declarations, ahead of every function definition),
	// mark it referenced, and return its symbol.
	std::string demand_array_dtor(DataDefCLASS *cdd, size_t n);
	node_t synth_array_dtor_def(DataDefCLASS *cdd, size_t n,
				    const std::string &sym);
	// Wrapper defs demanded during body translation, flushed at Pass 1.95.
	std::map<std::string, node_t> m_array_dtor_defs;
	// --finstrument-functions (task #66): the once-per-module exit thunk
	// `__madc_cyg_exit_thunk` — the cleanup attribute's handler that calls
	// __cyg_profile_func_exit with the instrumented function's own address.
	// Demanded by the first instrumented func_def, flushed at Pass 1.95.
	node_t m_instr_thunk_def = NULL;
	node_t synth_instr_exit_thunk();
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
	// Queue one global's init statements as a ctor GROUP (m_ctor_groups +
	// m_global_ctor_stmts). A linkonce (C++ `inline`) variable's group is
	// wrapped in a linkonce once-guard so a merged multi-TU image runs its
	// dynamic init exactly once (the g++ guarded COMDAT-init model); the
	// guard variable's declaration rides `deferred_globals`.
	void queue_global_ctor_group(class Variable *v, std::vector<node_t> &stmts,
				     std::vector<node_t> &deferred_globals);

	// ---- Expression translation ----
	node_t translate_expr(TokenBase *tb);
	// Boolean-context operand ([conv]/4 contextual conversion): translate_expr
	// plus the class-object `operator bool` conversion when applicable.
	node_t translate_cond(TokenBase *cond);
	node_t cond_contextual_bool(TokenBase *cond);

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
	// Class-instance declaration statement (`Foo f(a,b)`, `string s = "x"`,
	// `iterator it = m.begin()`): storage decl + injected construction (the
	// 1->N C++ decl lowering), appended to `items`. Shared by
	// translate_block's statement loop and the for-init wrap.
	void class_decl_stmts(class TokenDecl *sdcl, DataDefCLASS *cdcl,
			      node_t items);
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
	// array_elems > 0 = the object is a fixed array of that many elements;
	// the pushed dtor is then the per-(class,N) array wrapper.
	void emit_try_body_cleanup_push(const char *varname, class DataDefCLASS *cdd,
					node_t items, class TokenBase *origin,
					size_t array_elems = 0);
	// Range-based for over a madc array (madc::value): `for (T x : arr) body`. The loop
	// variable is declared in the enclosing scope by the parser, so this only
	// emits the index loop + per-iteration element fill (php_array_get /
	// php_array_get_int) around the translated body.
	node_t translate_foreach(class TokenFOREACH *fe);
	node_t translate_foreach_loop(class TokenFOREACH *fe);
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
	// TU identity for the object-mode per-TU init symbol; call before
	// translate_module (harmless in JIT mode — unused there).
	void set_tu_name(const char *s) { m_tu_name = s ? s : ""; }
	// The synthesized per-TU init's symbol (object mode; empty = none).
	const std::string &tu_init_name() const { return m_tu_init_name; }
	// Pack-side c2mir check gate, drop arm (rung 1, layer 4): called by
	// madc_cir_freeze with the defective top-level child indices reported
	// by c2mir_check_tree on a COPY of the pristine translated tree. Drops
	// the matching stashed pack defs (DEFBODY revert), re-runs the callee
	// cascade, and splices every newly-dropped def out of the tree.
	// Returns defs dropped, or -1 when a defective item is not a droppable
	// drained def (a TU defect — the freeze must abort loudly).
	int pack_gate_drop(node_t tree, const std::vector<int> &bad_items);
	// Consumer-excluded symbols (emission split): the freeze erases these
	// from funcdef_locs so no DF_HAS_FOREST_BODY stamp points at them.
	const std::set<std::string> &pack_stamp_exclusions() const
		{ return pack_stamp_excluded; }
	// UN-CARRIABLE symbols (pack_dropped): no consumer-side home of ANY
	// kind — local-class hoists, rolled-back speculative instantiations,
	// non-reverted drops. The 6b ownerless DEFBODY writer must not plant
	// a body span for them either (a pattern-spelling span derives into
	// "Expecting a type argument to iterator_traits<>" — the consumer
	// RE-INSTANTIATES instead, its standing story).
	const std::set<std::string> &pack_uncarriable_syms() const
		{ return pack_dropped; }
	// Bind ctors/dtor/methods of externally-defined / extern-template classes
	// to their exported Itanium symbols (dlsym-verified, fills only EMPTY
	// emit_symbols). Runs early in translate_module; re-run at module end
	// under pack_recording because the drain's body materialization clears
	// the bindings the freeze must snapshot.
	void bind_external_class_symbols(Program *prog);

	// ---- Tree copy (two-tree / materialize-from-AST, Phase 1) ----
	// Deep-copy a cir_node subtree into FRESH arena nodes — the `tsubst` core
	// (no substitution yet). Each copy gets a fresh node_t base (fresh uid,
	// attr=NULL, a private ops list rebuilt by recursion) so c2mir's in-place
	// `attr` mutation never touches a shared/immutable node; the madc extension
	// fields (origin_id, datadef, typedef_name, error_msg, src_lang,
	// synth_from_origin) are carried over, and `tree1_origin` records the source
	// node. ONLY c2mir's `attr` (per-compile post-check scratch) is dropped —
	// every build-time madc field is preserved. Leaf scalars (and their interned
	// payloads, valid for the c2mir context's lifetime) ride in the copied union.
	// This is the safe-for-c2mir private materialization every later phase builds
	// on. See docs/plans/2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md.
	//
	// `subst` (when non-NULL) turns the copy into a `tsubst`: any node whose
	// `datadef` is a template-parameter placeholder (DataDefTemplateParam),
	// directly or under pointer / reference / const layers, is rewritten to the
	// concrete type the map binds it to. A plain copy passes NULL and is
	// byte-identical to the pre-Phase-3 behaviour.
	cir_node *copy_cir_subtree(cir_node *src,
				   const std::map<DataDef *, DataDef *> *subst = nullptr);

	// Re-lower a pattern-mode deferred dependent construction (an N_IGNORE
	// marker whose ctor arguments contain a pack expansion, so ctor overload
	// selection is per-instantiation) into a concrete constructor call at
	// tsubst-copy time: expand the pack into concrete argument types, select
	// the ctor overload, build already-translated argument nodes per element
	// (copy under the element substitution), assemble via ctor_call_assemble
	// on `this_addr()`. Shared by the placement-new marker (`this_addr` = the
	// placement expression; `yield_this_addr` wraps the block as a statement
	// expression yielding the address) and the local-declaration marker
	// (`this_addr` = &var; a plain statement block). `relax_class_args`
	// (the decl path) admits non-pack class-object arguments (`*this` bound
	// to a reference parameter) and forces the manual assembly even when no
	// pack element demands it. Returns NULL when the caller's simple
	// re-translate path should run instead (placement-new only).
	// `require_overload_match` (the delegating-ci path) suppresses the
	// blind default-ctor fallback: a delegation names a SPECIFIC target
	// overload, so no-match must error (clean caller fallback), never
	// default-construct.
	cir_node *tsubst_relower_deferred_construction(
		const std::vector<class TokenBase *> &ctor_args,
		class TokenBase *origin,
		DataDefCLASS *concrete_class,
		const std::map<DataDef *, DataDef *> *subst,
		const std::function<node_t()> &this_addr,
		bool yield_this_addr,
		bool relax_class_args,
		bool require_overload_match = false);

	// Scalar-target twin of the deferred-construction relower: the placement-new
	// marker whose `_Up` substituted to a NON-class type ([expr.new] scalar
	// initialization = a store through the placement pointer). Claims the
	// value-init (no args) and single-pack-expansion (`_Up(std::forward<_Args>(
	// __args)...)` with pack arity 0/1) shapes and lowers them structurally from
	// the pack binding — never by re-translating the shared raw pattern tokens,
	// whose call identities an earlier instantiation may have baked. Returns
	// NULL when the shape is not claimable (caller keeps its fallback).
	cir_node *tsubst_scalar_placement_store(
		class TokenNEW *tn, DataDef *concrete,
		const std::map<DataDef *, DataDef *> *subst,
		const std::function<node_t()> &placement_addr);

	// `tsubst` proper (two-tree Phase 3): copy an immutable Tree-1 subtree into a
	// fresh per-instantiation Tree-2 subtree AND substitute its template-parameter
	// placeholders with concrete types (`subst`: placeholder DataDef* -> concrete
	// DataDef*). This is g++'s `tsubst` over `DECL_SAVED_TREE`: the saved pattern
	// is never mutated; every instantiation gets its own substituted copy that
	// c2mir then compiles. Thin wrapper over copy_cir_subtree.
	cir_node *tsubst_cir(cir_node *src,
			     const std::map<DataDef *, DataDef *> &subst);
	std::string copied_pack_value_name(const char *name) const;
	node_t copied_reference_slot_arg(class TokenBase *arg, node_t src_arg,
					 DataDef *formal, bool refp);
	node_t copied_call_arg_for_formal(class TokenBase *arg, node_t src_arg,
					 DataDef *formal, bool refp,
					 const std::map<DataDef *, DataDef *> *subst);
	class Variable *resolve_copied_dependent_call(
		class TokenCallFunc *tcf,
		const std::map<DataDef *, DataDef *> *subst,
		bool *changed_out = nullptr,
		std::vector<DataDef *> *concrete_param_types = nullptr,
		std::string *error_out = nullptr);
	bool system_header_pack_element_call_resolves(
		class TokenPackExpansion *pe,
		const std::map<DataDef *, DataDef *> &subst,
		class DataDefTemplateParam *tp,
		DataDef *elem,
		size_t elem_index,
		class DataDefCLASS *target);
	void rename_copied_pack_value_id(cir_node *src, cir_node *dst);
	void rewrite_copied_dependent_call_id(cir_node *src, cir_node *dst,
					      const std::map<DataDef *, DataDef *> *subst);
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

// First error_msg in the tree (pre-order), or NULL. Surfaces the per-call
// resolve-failure string that caused a tsubst pattern copy to be rejected.
const char *cir_first_error_msg(node_t tree);

// Collect every N_CALL callee symbol in the tree into `out`. Used to re-record
// the concrete callees of a tsubst-copied body as ODR-used (referenced_funcs)
// so the translate_module drain materializes their deferred-lazy definitions.
void cir_collect_call_callees(node_t tree, std::set<std::string> &out);

// Parameter NAMES of a FUNC_DEF node. The pack callee-cascade subtracts them
// from the harvested callees: a call through a fn-pointer parameter is
// indirect, not an external symbol to judge.
void cir_collect_funcdef_param_names(node_t fd, std::set<std::string> &out);

// Collect every __attribute__((cleanup(F))) function symbol in the tree.
// FOREST materialization sites only (a loaded body is pre-built — the live
// lowering site that registers F as referenced never runs for it).
void cir_collect_cleanup_attr_fns(node_t tree, std::set<std::string> &out);
void cir_collect_addr_fn_refs(node_t tree, std::set<std::string> &out);	// v25

#endif // __CIR_BUILDER_H
