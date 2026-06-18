#ifndef __MADC_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc main header file			2019 Derek Snider	//
//									//
//////////////////////////////////////////////////////////////////////////
#define __MADC_H 1

#include <cstdint>
#include <fstream>
#include <functional>
#include <istream>
#include <map>
#include <unordered_map>
#include <memory>
#include <ostream>
#include <sstream>
#include <deque>
#include <set>
#include <stack>
#include <vector>

#include "libmadc/value.h"

class Method;
class Program;
class MadcEngine;
class TokenBase;
class TokenSWITCH;
struct DelimDepth;	// parser-internal balanced-delimiter depth (parser.cpp)
class TokenCASE;

class MadcTeeBuf : public std::streambuf
{
public:
    std::streambuf *primary;
    std::streambuf *secondary;

    MadcTeeBuf(std::streambuf *p=NULL, std::streambuf *s=NULL)
	: primary(p), secondary(s) {}

protected:
    virtual int overflow(int ch = EOF) override;
    virtual std::streamsize xsputn(const char *s, std::streamsize n) override;
    virtual int sync() override;
};

class FuncDef: public DataDef
{
public:
    // The function's return TYPE. With first-class references a reference return
    // is a DataDefREF (is_reference() true); use returns_reference() to test it
    // and return_value_type() to get the referent. Read through those accessors,
    // not `returns` directly, so the reference identity lives in ONE place (the
    // type) — first-class refs Phase 2 retired the old parallel returns_ref flag.
    DataDef &returns;
    // True when the return type is a reference (T& / T&&). The reference lives in
    // the type (returns is a DataDefREF). A T& return is lowered to a by-address
    // (T*) return at codegen; the call site is an lvalue (assign stores through
    // it, read derefs it), matching g++.
    bool returns_reference() const { return returns.is_reference(); }
    // The VALUE type a (possibly reference) return denotes: the referent for a
    // reference return, else the return type itself. Flip-transparent — correct
    // whether `returns` holds the DataDefREF or (legacy) the bare referent.
    DataDef &return_value_type() const {
	if ( returns.is_reference() )
	    return *static_cast<DataDefPTR &>(returns).base_type;
	return returns;
    }
    std::vector<DataDef *> parameters;
    size_t explicit_alignment;
    // [&] capture support
    bool has_captures;
    struct CaptureEntry { std::string name; DataDef *type; };
    std::vector<Variable *> potential_captures; // outer-scope vars at lambda creation time
    std::vector<CaptureEntry> captures;         // populated during lambda body compilation
    // GNU nested-function / [&]-lambda capture-by-reference lowering: the
    // enclosing variables the body actually USES, in first-reference order.
    // Filled by the CIR builder while translating the body (pointer identity
    // against potential_captures). Each becomes a hidden `T *name` parameter and
    // every call site forwards `&var`. Empty for a non-capturing function.
    std::vector<Variable *> captured_vars;
    // The C symbol a madc-EMITTED function's body is DEFINED-as and CALLED-as,
    // when it differs from the default scheme. Two disjoint sources feed it
    // (a FuncDef is never both a hoisted free fn and a class method):
    //   - a GNU nested function / [&]-lambda hoisted to a unique top-level C
    //     symbol (`enclosing__name__N`); its in-scope alias keeps the source
    //     name, but every call site must reference this hoisted symbol.
    //   - a madc-emitted class method/operator whose default
    //     `ClassName__method` scheme collides with another overload of
    //     DIFFERENT arity (unary vs binary `operator-`, prefix vs postfix
    //     `operator++(int)`); this is the arity-disambiguated symbol.
    // Empty for the common case (call sites use the default scheme). DISTINCT
    // from emit_symbol: this still takes the normal madc-emitted-body path; it
    // is NOT the extern-binding (no-body) path that emit_symbol triggers.
    std::string local_emit_name;
    // multiple return values (empty = single return via `returns`)
    std::vector<DataDef *> return_types;
    // Reference-ness of parameter i, derived from its type: a reference parameter
    // is a DataDefREF (is_reference() true). This is the SINGLE source of truth —
    // the old parallel `ref_params` flag vector was retired (first-class-references
    // Phase 2). A DataDefREF renders its name as `T*`, so the emitted ABI is
    // unchanged; class_behind() unwraps it for dispatch.
    bool is_ref_param(size_t i) const {
	return i < parameters.size() && parameters[i]
	    && parameters[i]->is_reference();
    }
    // const parameter tracking: const_params[i] == true when parameter i is const T&
    std::vector<bool> const_params;
    // Canonical C++ spelling of each parameter, captured from the SOURCE TOKENS
    // at parse time (where top-level pointee-const / `&` / template spelling are
    // all visible — the DataDef alone loses pointee-const, e.g. it stores
    // "char*" for a `const char*` param). Index-aligned with `parameters`; the
    // hidden `__this` slot (param 0 of a method) holds an empty string. Fed to
    // the Itanium mangler so a header-declared C++ method binds to its real
    // external symbol.
    std::vector<std::string> param_cpp_spellings;
    // Source typedef alias used for each parameter, when the declaration named
    // one. Index-aligned with `parameters`; empty means render from DataDef.
    std::vector<std::string> param_typedef_names;
    // Default ARGUMENT expression for each parameter (C++ `T x = expr`), captured
    // at parse time, index-aligned with `parameters`; NULL when the parameter has
    // no default. A call that omits a trailing argument fills it from here, and
    // arity matching treats the function as callable with [required..total] args
    // (required = count of params with no default).
    std::vector<class TokenBase *> param_defaults;
    // Number of leading parameters that have NO default — the minimum arg count a
    // call must supply. Equals parameters.size() when no parameter has a default.
    size_t required_param_count() const
    {
	size_t req = parameters.size();
	for ( size_t i = parameters.size(); i-- > 0; )
	{
	    if ( i < param_defaults.size() && param_defaults[i] )
		req = i;
	    else
		break;
	}
	return req;
    }
    std::string template_return_param_name;
    int template_return_deduce_arg_index;
    bool template_return_deduce_from_pointer;
    bool template_return_ref;
    std::string return_typedef_name;
    // When non-empty, the C symbol this function is CALLED as / DEFINED as,
    // instead of the default ClassName__method scheme. Used to bind a class
    // method directly to an externally-provided C++ ABI symbol. madc emits no
    // body for such methods.
    std::string emit_symbol;
    // The UNMANGLED display name of a class method (`take`, `find`, `operator=`),
    // independent of the mangled call symbol stored as the Variable's name
    // (`Box__take`, `Box__take__o2`). Lets overload resolution enumerate the
    // same-named overloads in DataDefCLASS::methods (whose entries are keyed by
    // their mangled names). Empty for non-method FuncDefs.
    std::string method_display_name;
    // Namespace/free-function source identity for overloaded C++ functions.
    // These let call-site resolution enumerate overloads registered under unique
    // internal symbols while preserving the source name.
    std::string function_display_name;
    std::string namespace_name;
    std::string inline_builtin_kind;
    // For an externally-bound ctor (emit_symbol set) whose real ABI takes a
    // trailing reference argument that madc has no value for, pass the object's
    // own address (&this) as that trailing arg.
    bool ctor_trailing_self;
    // Member function/constructor template specializations are emitted in the
    // current translation unit unless an explicit specialization says otherwise;
    // an extern-template class instantiation does not export arbitrary member
    // template specializations.
    bool is_member_template;
    std::vector<std::string> template_param_names;
    // Pack-ness per template_param_names entry (a `typename... _Args` parameter
    // pack vs a plain `typename _Up`). Needed so a variadic member template
    // (allocator_traits::construct) instantiates its `_Args...` correctly.
    // Empty == none-are-packs (back-compat for older registrations).
    std::vector<bool> template_param_is_pack;
    std::string template_return_spelling;
    std::vector<std::string> template_param_spellings;
    // For a STATIC member function template of a madc-LOCAL (monomorphized,
    // not libstdc++-exported) class, the retained body declaration tokens
    // (declarator + params + `{ ... }`, WITHOUT the `template<...>` header) and
    // the owning class. A call site instantiates the body on ODR-use
    // (instantiate_member_fn_template_for_call) instead of leaving a bare
    // undefined extern. Empty unless the body was retained.
    std::vector<TokenBase *> member_template_decl;
    DataDef *member_template_owner;
    // The DEPENDENT return-type token range of a member function template
    // (the tokens before the declarator name, still naming the template
    // parameters — e.g. `Succ < T >`). Retained for BOTH body-bearing AND
    // body-less member templates so a call site can resolve the CONCRETE
    // return type by substituting the deduced/explicit args into these tokens
    // WITHOUT instantiating a body (the clang SubstDecl model — see
    // resolve_member_template_call_return_type). Empty == not a member tmpl.
    std::vector<TokenBase *> member_template_return_tokens;
    struct CtorInitializer {
	std::string name;
	std::vector<TokenBase *> args;
    };
    std::vector<CtorInitializer> ctor_initializers;
    // Initializer order matches member declaration order (avoids -Wreorder).
    FuncDef(DataDef &d) : returns(d), explicit_alignment(0), has_captures(false), template_return_param_name(), template_return_deduce_arg_index(-1), template_return_deduce_from_pointer(false), template_return_ref(false), return_typedef_name(), emit_symbol(), method_display_name(), function_display_name(), namespace_name(), inline_builtin_kind(), ctor_trailing_self(false), is_member_template(false), template_param_names(), template_param_is_pack(), template_return_spelling(), template_param_spellings(), member_template_decl(), member_template_owner(NULL), member_template_return_tokens(), ctor_initializers(), is_varargs(false), is_void_params(false), no_instrument_function(false), no_strict_aliasing(false), has_large_struct_retbuf(false), declaration_only(false), defaulted_or_deleted(false), is_deleted(false), pure_virtual(false), is_const_method(false) {}
    DataDef *findParameter(std::string &);
    virtual BaseType basetype() const { return BaseType::btFunct; }
    virtual size_t alignment() const { return explicit_alignment ? explicit_alignment : DataDef::alignment(); }
    bool is_varargs;  // function declared with ... (variadic)
    bool is_void_params; // f(void) — explicitly zero params (vs f() which is K&R unspecified)
    bool no_instrument_function;
    // __attribute__((optimize("-fno-strict-aliasing"))): the CIR builder forwards
    // this as an N_ATTR in the FUNC_DEF specs; c2mir suppresses TBAA per-function.
    bool no_strict_aliasing;
    bool has_large_struct_retbuf; // __retbuf was injected for struct return > 16 bytes
    // True when DECLARED with no body (prototype ended in ';' / ',' not '{').
    // For a C++ class method whose class carries canonical C++ spelling, this
    // can bind emit_symbol to the mangled external symbol. Stays false for any
    // madc-compiled (bodied) function.
    bool declaration_only;
    // The source file of a declaration_only prototype (token ->file pointer;
    // NULL when not recorded). Lets the compile-stage registration-policy
    // gate distinguish a USER-SOURCE extern prototype from curated header
    // declarations.
    const char *decl_file = nullptr;
    // True for C++ special declarations like `= default` or `= delete`.
    // These are not bodyless shared-library declarations and must not be bound
    // as external symbols just because the class has canonical C++ spelling.
    bool defaulted_or_deleted;
    // True ONLY for `= delete` (a SUBSET of defaulted_or_deleted, which also
    // covers `= default`). Distinguished because faithful `__is_assignable` /
    // `__is_constructible` must treat a deleted special member as "not
    // assignable / not constructible" while a defaulted one is available.
    bool is_deleted;
    // True for C++ pure virtual declarations (`= 0`). They have no body but
    // still participate in method lookup and vtable layout.
    bool pure_virtual;
    // True when the method was declared with a trailing `const` (e.g.
    // `bool good() const;`). The const-qualified `this` mangles with the Itanium
    // 'K' (e.g. _ZNKSt9basic_ios...4goodEv). Set by TokenCLASS::parse / parseFunction
    // when a trailing const follows the parameter list. Default false.
    bool is_const_method;
    bool is_multi_return() const { return return_types.size() > 1; }
};

// Generic overload-resolution ranking — NO type is special-cased. Ranks binding
// an argument of type `adc` to a parameter of type `pdc`: higher is a better
// match; -1 means it cannot bind. Scalars, pointers, and ANY class object all
// go through the same category logic; a class parameter additionally accepts an
// argument through ONE user-defined conversion (a single-argument converting
// constructor of that class). That converting-ctor path is what lets, say, a
// `const char*` bind a class parameter that has a `(const char*)` constructor
// WITHOUT the resolver knowing or caring which class it is. `allow_udc` is
// cleared on the recursive converting-ctor check so an implicit conversion
// sequence uses at most one user-defined conversion (the C++ rule), which also
// bounds the recursion to depth 1. Defined in cir_builder.cpp.
// `arg_is_zero_literal`: the argument EXPRESSION is an integer literal of value
// zero — a C++ null-pointer constant ([conv.ptr]), so it binds a pointer
// parameter as a standard conversion. Only callers that can see the argument
// token set it; the DataDef alone cannot carry "literal zero".
int score_arg_to_param(const DataDef *adc, const DataDef *pdc,
		       bool param_is_ref = false, bool allow_udc = true,
		       bool arg_is_zero_literal = false);

class DataStruct: public DataDef
{
public:
    std::vector<DataDef *> elements;
    virtual BaseType basetype() const { return BaseType::btStruct; }
};

class DataClass: public DataDef
{
public:
    std::vector<DataDef *> elements;
    std::vector<FuncDef *> methods;
    virtual BaseType basetype() const { return BaseType::btClass; }
};

class Method
{
public:
    Variable &returns;
    std::vector<Variable *> parameters;
    std::vector<Variable *> variables;
    void *x86code;
    Variable *env_param; // hidden void** param for [&] lambdas (nullptr if no capture)
    class DataDefCLASS *owner_class; // non-null when this is a class method
    Method(Variable &v) : returns(v), x86code(NULL), env_param(NULL), owner_class(NULL) {}
    Variable *getParameter(unsigned int i) { if ( i >= parameters.size() ) return NULL; return parameters[i]; }
    Variable *findParameter(std::string &);
    Variable *findVariable(std::string &);
};


// Program tokens


// placeholder class
class TokenStmt: public TokenBase
{
public:
    TokenStmt() : TokenBase() {}
};


// Token for containing a "Compound Statement" built up of multiple "Statement" tokens
// Used to to extend the "Function" token and the "Program" token, described below
//
// Used to keep track of local variables, and nesting of brackets { }
// As well as managing registers, and stack management (constructors, destructors)
class TokenCpnd: public virtual TokenBase
{
public:
    Method *method;
    TokenCpnd *parent;
    TokenCpnd *child;
    std::vector<Variable *> variables;
    // O(1) name -> Variable* index over `variables`, built incrementally. The old
    // linear scan in findVariable was the dominant parse cost on large real system
    // headers (thousands of symbols per scope -> O(n^2)). `var_indexed` = how many
    // of `variables` have been absorbed; findVariable absorbs any appended since,
    // first-wins (emplace) to match the old front-to-back scan. `variables` is
    // append-only during parse; the one erase site (foreach-catch lowering) resets
    // the index, and a shrink is detected defensively below.
    std::unordered_map<std::string, Variable *> var_index;
    size_t var_indexed = 0;
    std::vector<TokenStmt *> statements;
    std::vector<TokenBase *> deferred;   // defer statements (compiled in LIFO at scope exit)
    std::vector<Variable *> destruct_order; // class-typed vars in declaration order (for LIFO dtor)
    int end_line;			// line of closing } (set by parseCompound)
    bool is_stmt_expr = false;		// true: a GNU statement-expression `({...})`, not a plain `{...}` block
    TokenCpnd() : TokenBase() { method = NULL; parent = NULL; child = NULL; end_line = 0; }
    virtual TokenType type() const { return TokenType::ttCompound; }
    virtual DataDef *datadef() const override {
	if ( statements.empty() ) return &ddVOID;
	DataDef *dd = statements.back()->datadef();
	return dd ? dd : &ddVOID;
    }
    Variable *getParameter(unsigned int);
    Variable *findParameter(std::string &s);
    Variable *findVariable(std::string &);
};

class TokenFunc: public TokenVar, public TokenCpnd
{
public:
    // True when a later definition of the same function overrides this
    // one. Set during compile pre-pass by walking pending_funcs in
    // reverse and marking earlier duplicates. Overridden TokenFuncs
    // skip both prepareFuncNode and body emission so asmjit's Compiler
    // sees exactly one addFunc per funcnode — without this, calling
    // addFunc(node) twice for the same FuncNode causes asmjit to lose
    // track of every other funcnode added between the duplicate calls,
    // leaving their labels unbound.
    bool is_overridden = false;
    TokenFunc(Variable &v) : TokenVar(v), TokenCpnd() {}
    virtual size_t argc() const { if (var.type->basetype() != BaseType::btFunct) return 0; return ((FuncDef *)var.type)->parameters.size(); }
    virtual TokenType type() const { return TokenType::ttFunction; }
};

class TokenDecl: public TokenVar
{
public:
    TokenBase *initialize;
    std::vector<TokenBase *> init_list; // brace-enclosed initializer for fixed-size arrays
    std::vector<TokenBase *> ctor_args; // constructor arguments for class-typed vars
    bool has_brace_init;               // true when `= { ... }` syntax was used
    bool is_const_decl;                // true when declared with `const` qualifier
    // True when this declaration carried a constant initializer that the parser
    // baked into Variable::data and then cleared init_list (a static/global
    // fixed array — brace `{...}` OR string-literal `"..."` char array). The CIR
    // builder uses this to reconstruct the INIT list from v->data. Crucially it
    // distinguishes the DEFINING declaration from a shared `extern` placeholder
    // (which never had an initializer), so the extern is not turned into a
    // second definition ("Repeated item declaration").
    bool baked_static_init = false;
    // True when this is a FUNCTION-BLOCK-scope `extern T name;` that refers to a
    // file-scope global of the same name. The CIR backend must emit an actual
    // `extern T name;` inside that block so c2mir's scope resolution rebinds the
    // name to the file-scope object — otherwise an enclosing local of the same
    // name would shadow it (C: a block `extern` re-exposes the file-scope
    // object). Distinct from baked_static_init: this NEVER defines storage.
    bool block_extern_redecl = false;
    TokenDecl(Variable &v) : TokenVar(v) { initialize = NULL; has_brace_init = false; is_const_decl = false; }
    virtual TokenType type() const { return TokenType::ttDeclare; }
};

// AST node for a typedef declaration. Returned by TokenTYPEDEF::parse()
// so typedefs appear in the AST in source order (consumed by the CIR
// layer via Program::top_decls). The JIT compiler ignores it: compile()
// is a no-op that returns the inherited _operand.
class TokenTypedefDecl: public TokenBase
{
public:
    std::string alias;       // typedef alias name (e.g. "EXT_BV")
    DataDef *target_type;    // what the typedef resolves to
    TokenTypedefDecl(const std::string &a, DataDef *t) : alias(a), target_type(t) {}
    virtual TokenType type() const { return TokenType::ttTypedefDecl; }
};

// AST node for a standalone struct/union definition (no variable
// declarator). Returned by TokenSTRUCT::parse() so the definition keeps
// its source-order position. The JIT compiler ignores it.
class TokenStructDef: public TokenBase
{
public:
    DataDefSTRUCT *sdd;
    bool is_union;
    TokenStructDef(DataDefSTRUCT *s, bool u = false) : sdd(s), is_union(u) {}
    virtual TokenType type() const { return TokenType::ttStructDef; }
};

// { v0, v1, ... } — a nested brace initializer, used for elements of an
// array-of-structs or for nested struct members. Not a value by itself.
class TokenStructLit: public TokenBase
{
public:
    std::vector<TokenBase *> inits;
    // When the compound-literal type was named via a typedef (e.g. `(T){...}`),
    // this holds the alias so the CIR builder can render the type-name spec as
    // ID("T") instead of re-deriving an anonymous struct/union layout. Empty for
    // a bare `struct Tag` / `union Tag` / builtin element type.
    std::string typedef_name;
    // Non-null for a C99 ARRAY compound literal `(T[]){...}` / `(T[N]){...}`.
    // The parser models the literal's storage as a synthetic `__compound_array`
    // struct (datadef()), but that loses array semantics: c2mir would see a
    // forward-ref struct (incomplete type) that cannot be subscripted. When this
    // is set the CIR builder instead emits a real array type-name
    // `T name[]` so c2mir sizes the array from the initializer count — the
    // faithful C99 lowering. Holds the ELEMENT type T.
    DataDef *array_elem_dd = nullptr;
    TokenStructLit() {}
    virtual TokenType type() const { return TokenType::ttStructLit; }
};

class TokenCallFunc: public TokenVar
{
public:
    std::vector<TokenBase *> parameters;
    DataDef *return_override = nullptr;
    bool returns_ref_override = false;
    // Explicit template arguments captured at the call site
    // (`__stoa<long, int>(...)`): leading template parameters bound
    // left-to-right by instantiate_namespace_fn_template_for_call.
    // Empty for ordinary calls.
    std::vector<DataDef *> explicit_template_args;
    // Number of USER-WRITTEN arguments, set when parseCallFunc appends
    // C++ default arguments. Overload ranking must consider only these:
    // the defaults were copied from ONE overload's declaration, and scoring
    // them (e.g. the literal 0 of `size_t* __idx = 0` against the pointer
    // param) would veto the very overload that supplied them.
    // (size_t)-1 = no defaults appended; every argument is user-written.
    size_t user_argc = (size_t)-1;
    // Non-null when the function-pointer value comes from a sub-expression
    // (e.g. a struct member access c.fn or arr[i].fn) rather than a variable.
    // When set, TokenCallFunc::compile loads the fn-ptr by compiling src_node
    // instead of calling voperand(var). var.type must still be DataDefFPTR.
    TokenBase *src_node = nullptr;
    bool auto_scope_context = false;
    TokenCallFunc(Variable &v) : TokenVar(v) { if (v.type->is_function()) _datatype = returns(); }
    virtual DataDef *returns()  const {
	if ( return_override )
	    return return_override;
	if ( DataDefFPTR *fptr = dynamic_cast<DataDefFPTR *>(var.type) )
	    return (fptr->target != NULL) ? &fptr->target->returns : &ddVOID;
	return &((FuncDef *)var.type)->returns;
    }
    virtual DataDef *datadef()  const override {
        if ( var.type->is_function() )
            return returns();
        return _datatype;
    }
    virtual bool is_real() const override { return datadef() && datadef()->is_real(); }
    virtual size_t argc() const { return parameters.size(); }
    virtual TokenType type() const { return TokenType::ttCallFunc; }
};

class TokenScopeContext: public TokenBase
{
public:
    Variable &context_var;
    std::vector<Variable *> scope_vars;
    TokenScopeContext(Variable &ctx) : TokenBase(), context_var(ctx) { _datatype = &ddARRAY; }
    virtual TokenType type() const { return TokenType::ttVariable; }
    virtual DataDef *datadef() const override { return &ddARRAY; }
};

class TokenMember: public TokenCallFunc
{
public:
    Variable &object;
    size_t offset;
    TokenBase *parent_expr;  // non-null for chained -> (e.g. ch->in_room->name)
    TokenMember(Variable &o, Variable &m, size_t ofs)
        : TokenCallFunc(m), object(o), offset(ofs), parent_expr(nullptr) { _datatype = m.type; }
    TokenMember(Variable &o, Variable &m, size_t ofs, TokenBase *parent)
        : TokenCallFunc(m), object(o), offset(ofs), parent_expr(parent) { _datatype = m.type; }
    virtual TokenType type() const { return TokenType::ttMember; }
    virtual DataDef *datadef() const override { return _datatype; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    // Member is declared as a fixed array (e.g. `SKILLTYPE *arr[N]`).
    // Such a member's datadef reports the element type but the storage
    // is in-place, so subscripting needs LEA on the member's Mem and
    // the parser must not unwrap pointer-typed elements.
    bool is_fixed_array_member() const
    {
	// For `obj->member` access, object.type is the pointer-to-struct,
	// not the struct itself. Walk through any pointer wrapper.
	DataDefSTRUCT *sdd = owner_struct_type();
	if ( !sdd ) return false;
	std::string mname = var.name;
	return sdd->m_is_array_decl(mname);
    }
    DataDefSTRUCT *owner_struct_type() const
    {
	DataDef *otype = object.type;
	if ( DataDefPTR *opt = dynamic_cast<DataDefPTR *>(otype) )
	    otype = opt->base_type;
	return dynamic_cast<DataDefSTRUCT *>(otype);
    }
    const DataDefSTRUCT::BitFieldInfo *bitfield_info() const
    {
	DataDefSTRUCT *sdd = owner_struct_type();
	if ( !sdd ) return NULL;
	return sdd->m_bitfield(var.name);
    }
    bool is_bitfield_member() const
    {
	return bitfield_info() != NULL;
    }
};

// & address-of operator — emits LEA to get address of a variable
class TokenAddrOf: public TokenBase
{
public:
    Variable &var;
    DataDef *ptr_type;  // pointer-to-var type
    TokenAddrOf(Variable &v, DataDef *pt) : var(v), ptr_type(pt) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return ptr_type ? ptr_type : &ddVOID; }
};

// &(expr) address-of operator for member/subscript/deref lvalues
class TokenAddrExpr: public TokenBase
{
public:
    TokenBase *expr;
    DataDef *ptr_type;
    TokenAddrExpr(TokenBase *e, DataDef *pt) : expr(e), ptr_type(pt) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return ptr_type ? ptr_type : &ddVOID; }
};

// GNU computed-goto label address: `&&label`
class TokenLabelAddr: public TokenBase
{
public:
    std::string name;
    DataDef *ptr_type;
    TokenLabelAddr(const std::string &n, DataDef *pt) : name(n), ptr_type(pt) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual TokenBase *clone() { return new TokenLabelAddr(name, ptr_type); }
    virtual DataDef *datadef() const override { return ptr_type ? ptr_type : &ddVOID; }
};

class TokenExprContextObject: public TokenBase
{
public:
    std::string path;
    const madc::value *context_value;
    TokenExprContextObject(const std::string &p, const madc::value *v)
	: path(p), context_value(v) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual TokenBase *clone() { return new TokenExprContextObject(path, context_value); }
};

// *ptr dereference — reads/writes the value at the address held by a pointer
class TokenDeref: public TokenBase
{
public:
    Variable &var;
    DataDef *deref_type;  // pointed-to type
    TokenDeref(Variable &v, DataDef *dt) : var(v), deref_type(dt) { _datatype = dt; }
    virtual TokenType type() const { return TokenType::ttMember; }  // reuse member type for assignment compat
    virtual DataDef *datadef() const override { return deref_type; }
};

// *(expr) dereference for cast/member/subscript pointer expressions
class TokenDerefExpr: public TokenBase
{
public:
    TokenBase *expr;
    DataDef *deref_type;
    TokenDerefExpr(TokenBase *e, DataDef *dt) : expr(e), deref_type(dt) { _datatype = dt; }
    virtual TokenType type() const { return TokenType::ttMember; }
    virtual DataDef *datadef() const override { return deref_type; }
};

class TokenComplexPart: public TokenBase
{
public:
    TokenBase *expr;
    bool imag_part;

    TokenComplexPart(TokenBase *e, bool imag)
	: expr(e), imag_part(imag) {}

    virtual TokenType type() const { return TokenType::ttMember; }
    virtual DataDef *datadef() const override
    {
	DataDef *expr_dd = expr ? expr->datadef() : NULL;
	if ( expr_dd && expr_dd->is_complex() )
	{
	    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(expr_dd);
	    return (cdd && cdd->element_type) ? cdd->element_type : expr_dd;
	}
	if ( imag_part )
	    return (expr_dd && expr_dd->is_real()) ? expr_dd : &ddINT64;
	return expr_dd ? expr_dd : &ddINT64;
    }
};

// *ptr++ / *ptr-- — dereference the current pointer value, then advance
// or rewind the pointer variable itself.
class TokenDerefStep: public TokenBase
{
public:
    Variable &var;
    DataDef *deref_type;
    bool increment;
    TokenDerefStep(Variable &v, DataDef *dt, bool inc)
        : var(v), deref_type(dt), increment(inc) { _datatype = dt; }
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return deref_type; }
};

// (TYPE *) cast expression — type annotation, no codegen for pointer casts
class TokenCast: public TokenBase
{
public:
    DataDef *cast_type;   // target type
    TokenBase *expr;      // expression being cast
    TokenCast(DataDef *ct, TokenBase *e) : cast_type(ct), expr(e) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return cast_type; }
};

class TokenCallMethod: public TokenMember
{
public:
    TokenCallMethod(Variable &o, Variable &m) : TokenMember(o, m, 0) { _datatype = returns(); }
    virtual TokenType type() const { return TokenType::ttCallMethod; }
};

// subscript access: container[index]
class TokenSubscript: public TokenBase
{
public:
    Variable &object;    // the container variable
    TokenBase *index;    // the primary (first) index expression
    std::vector<TokenBase *> extra_indices; // additional indices for multi-dim fixed arrays
    Variable *tmp_var;   // temp string variable for string-returning subscripts (or NULL)

    TokenSubscript(Variable &o, TokenBase *idx, Variable *tmp = nullptr)
        : object(o), index(idx), tmp_var(tmp)
    {
        if ( o.is_fixed_array() )
            _datatype = o.type; // C fixed array: subscript yields element of base type
        else if ( o.type->is_pointer() )
        {
            // Raw pointer: ptr[i] == *(ptr + i). Element type = pointed-to type.
            DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(o.type);
            _datatype = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
        }
        else if ( o.type->type() == DataType::dtSIMD )
            _datatype = static_cast<DataDefSIMD *>(o.type)->element_type;
        else if ( DataDef *e = subscript_operator_element_type(o.type) )
            // A class with `T& operator[](...)` (a real madc template container
            // like vector<T>/map<K,V>/set<T>): the element type is the operator[]
            // return VALUE type (the base T — return_value_type() yields the
            // referent of a reference return). This is what lets `v[i].method()`
            // see a structured element.
            _datatype = e;
        else
            _datatype = &ddINT64; // madc array (madc::value): default to int
    }

    // The element type produced by a class object's `operator[]`, or NULL when
    // `dd` is not a class declaring operator[]. Static so the ctor can use it.
    static DataDef *subscript_operator_element_type(DataDef *dd)
    {
        if ( !dd || !dd->is_object() )
            return NULL;
        DataDefCLASS *cls = dynamic_cast<DataDefCLASS *>(dd);
        if ( !cls )
            return NULL;
        std::string opname = "operator[]";
        Variable *mv = cls->findMethod(opname);
        if ( !mv )
            return NULL;
	FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
	if ( !fd )
	    return NULL;
	return &fd->return_value_type();
    }
    virtual TokenType type() const { return TokenType::ttSubscript; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    // The element type is computed in the constructor for every container
    // kind (fixed array, pointer, string, SIMD, vector, map, madc array).
    // The asmjit codegen path used a helper to refine multi-dimensional
    // fixed-array indexing; that path is gone, so return the stored
    // element type directly. The CIR backend derives types independently.
    virtual DataDef *datadef() const override { return _datatype; }
};

class TokenSubscriptExpr: public TokenBase
{
public:
    TokenBase *base_expr;
    TokenBase *index;

    TokenSubscriptExpr(TokenBase *base, TokenBase *idx, DataDef *elem_type)
        : base_expr(base), index(idx)
    {
        _datatype = elem_type ? elem_type : &ddINT64;
    }
    virtual TokenType type() const { return TokenType::ttSubscript; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    // operand() must return an *lvalue* (Mem) — i.e. the address of
    // the element — not the loaded value. Callers (TokenAssign LHS,
    // outer TokenSubscriptExpr for chained 2D indexing, TokenMember
    // for s[i].field, etc.) need to compute offsets from the
    // element's address, and the default `compile()` path emits
    // emit_ir_value which loads through Mem and yields a Gp holding
    // the value — chaining through that returned a numeric value as
    // if it were an address and crashed at NULL/garbage.
};

class TokenProgram: public TokenCpnd
{
public:
    std::string source;
    std::istream *is;
    uint32_t lines;
    size_t bytes;
    TokenProgram() : TokenCpnd() { lines = 0; bytes = 0; is = NULL; }
    virtual TokenType type() const { return TokenType::ttProgram; }
};


// generic void function pointer
typedef void (*fVOIDFUNC)(void);

// maps
typedef std::map<std::string, TokenKeyword *> keyword_map_t;
typedef std::map<std::string, TokenDataType *> datatype_map_t;
typedef std::map<std::string, DataDef *> datadef_map_t;
typedef std::map<std::string, FuncDef *> funcdef_map_t;
typedef std::map<std::string, Variable *> variable_map_t;
typedef std::map<std::string, variable_map_t> namespace_map_t;
typedef std::map<std::string, datatype_map_t> namespace_datatype_map_t;

// map-iterators
typedef std::map<std::string, TokenKeyword *>::iterator keyword_map_iter;
typedef std::map<std::string, TokenDataType *>::iterator datatype_map_iter;
typedef std::map<std::string, DataDef *>::iterator datadef_map_iter;
typedef std::map<std::string, FuncDef *>::iterator funcdef_map_iter;
typedef std::map<std::string, Variable *>::iterator variable_map_iter;

// A builtin-registration type selector: names a parameter/return type for
// addFunction() either by a DataType enum word or directly by a DataDef*. Both
// constructors are implicit so existing `datatype_vec_t{dtX,...}` call sites are
// unchanged, while class types can be named by their actual DataDef. When `dd`
// is set it wins; otherwise `dt` is resolved by the existing DataType_to_dd
// switch.
struct typespec_t
{
    DataType  dt;
    DataDef  *dd;
    RefType   ref;   // when dd!=NULL: rtValue, rtPointer (T*), or rtReference (T&)
    typespec_t(DataType t) : dt(t), dd(nullptr), ref(RefType::rtValue) {}
    typespec_t(DataDef *d) : dt(DataType::dtVOID), dd(d), ref(RefType::rtValue) {}
    typespec_t(DataDef &d) : dt(DataType::dtVOID), dd(&d), ref(RefType::rtValue) {}
    typespec_t(DataDef *d, RefType r) : dt(DataType::dtVOID), dd(d), ref(r) {}
};

inline typespec_t ptr_of(DataDef &d) { return typespec_t(&d, RefType::rtPointer); }
inline typespec_t ref_of(DataDef &d) { return typespec_t(&d, RefType::rtReference); }

// vectors
typedef std::vector<typespec_t> datatype_vec_t;
typedef std::vector<Variable *> variable_vec_t;

// vector iterators
typedef std::vector<DataDef *>::iterator datadef_vec_iter;
typedef std::vector<Variable *>::iterator variable_vec_iter;
//typedef std::vector<CodeBlock *>::iterator codeblock_vec_iter;
typedef std::vector<TokenBase *>::iterator tokenbase_vec_iter;


// class to hold source for lexing
// embedded header lookup (generated by scripts/gen_embedded_headers.sh)
const std::string *find_embedded_header(const std::string &name);

class Source
{
protected:
    struct PushbackFrame
    {
	size_t remaining;
	std::string disabled_macro;
	bool recount = true;   // false: text was already read once; re-reading
			       // it must not re-advance the column counter
    };
    std::stringstream _ss;
    std::string _pushback;		// pushback buffer for #define substitution
    std::deque<PushbackFrame> _pushback_frames;
    int _lf, _cr, _column;
    std::streampos _pos;
    std::string _fname;
    void add_pushback_frame(const std::string &s, const std::string &disabled_macro,
			    bool recount = true)
    {
	if ( s.empty() )
	    return;
	PushbackFrame frame;
	frame.remaining = s.size();
	frame.disabled_macro = disabled_macro;
	frame.recount = recount;
	_pushback_frames.push_front(frame);
    }
public:
    Source() { _lf = 0; _cr = 0; _column = 0; _pos = 0; }
    const char *fname() const { return _fname.c_str(); }
    const char *fname(const char *s)  { _fname = s; return _fname.c_str(); }
    const char *fname(std::string &s) { _fname = s; return _fname.c_str(); }
    void copybuf(std::streambuf *sb)  { _ss << sb;  }
    void str(const std::string &s) { _ss.str(s); }
    void pushback(const std::string &s) { _pushback = s + _pushback; add_pushback_frame(s, ""); }
    // Push back text that was ALREADY read (lexer lookahead/backtrack). Those
    // source characters were already counted on the first read, so re-reading
    // them must not re-advance the column counter (contrast pushback(), used
    // for synthesized text like macro expansions, which does count).
    void pushback_reread(const std::string &s) { _pushback = s + _pushback; add_pushback_frame(s, "", false); }
    void pushback_macro(const std::string &s, const std::string &disabled_macro)
    {
	_pushback = s + _pushback;
	add_pushback_frame(s, disabled_macro);
    }
    bool macro_disabled(const std::string &name) const
    {
	for ( const PushbackFrame &frame : _pushback_frames )
	    if ( frame.disabled_macro == name )
		return true;
	return false;
    }
    // Active macro-expansion nesting depth (one frame per live expansion).
    // A runaway recursive macro grows this without bound; the lexer guards on
    // it to fail with a clean diagnostic instead of crashing the stack.
    size_t pushback_depth() const { return _pushback_frames.size(); }
    bool good() { return !_pushback.empty() || _ss.good(); }
    bool eof()  { return _pushback.empty() && _ss.eof(); }
    int line()  { if ( _lf > _cr ) return _lf+1; return _cr+1; }
    int column(){ return _column ? _column : 1; }
    int get()
    {
	if ( !_pushback.empty() )
	{
	    // Delayed-pop blue paint. A macro-expansion frame whose text was fully
	    // consumed on a PRIOR get() is dropped HERE, on the next read — NOT the
	    // instant its last char was taken. That keeps the macro "disabled"
	    // (macro_disabled()) while the token its expansion produced is re-lexed
	    // and macro-checked, so a self-referential `#define A A` (or mutual
	    // `A`<->`B`) expands once and stops instead of recursing to a crash.
	    while ( !_pushback_frames.empty() && _pushback_frames.front().remaining == 0 )
		_pushback_frames.pop_front();
	    int ch = (unsigned char)_pushback[0];
	    _pushback.erase(0, 1);
	    bool recount = true;
	    if ( !_pushback_frames.empty() )
	    {
		recount = _pushback_frames.front().recount;
		if ( _pushback_frames.front().remaining > 0 )
		    --_pushback_frames.front().remaining;
	    }
	    if ( recount )
		++_column;
	    return ch;
	}
	// Pushback drained: any frames left are fully-consumed expansions — drop
	// them so a later, independent use of the same macro can expand again.
	if ( !_pushback_frames.empty() )
	    _pushback_frames.clear();
	int ch = _ss.get();
	if ( ch == -1 ) { return -1; }
	// C line splice: backslash + optional trailing whitespace + newline
	if ( ch == '\\' )
	{
	    std::streampos splice_start = _ss.tellg();
	    // skip optional trailing spaces/tabs after backslash
	    while ( _ss.peek() == ' ' || _ss.peek() == '\t' )
		_ss.get();
	    int next = _ss.peek();
	    if ( next == '\n' || next == '\r' )
	    {
		_ss.get();
		if ( next == '\r' && _ss.peek() == '\n' )
		    _ss.get();
		++_lf; _column = 0; _pos = _ss.tellg();
		return get(); // recurse to get next real char
	    }
	    // not a line splice — rewind
	    _ss.seekg(splice_start);
	}
	/**/ if ( ch == '\n' ) { ++_lf; _column = 0; _pos = _ss.tellg(); }
	else if ( ch == '\r' ) { ++_cr; _column = 0; _pos = _ss.tellg(); }
	else { ++_column; }
	return ch;
    }
    int peek()
    {
	if ( !_pushback.empty() )
	    return (unsigned char)_pushback[0];
	// C line splice: skip backslash + optional whitespace + newline in peek
	int ch = _ss.peek();
	if ( ch == '\\' )
	{
	    std::streampos saved = _ss.tellg();
	    _ss.get(); // consume '\'
	    // skip optional trailing spaces/tabs
	    while ( _ss.peek() == ' ' || _ss.peek() == '\t' )
		_ss.get();
	    int next = _ss.peek();
	    if ( next == '\n' || next == '\r' )
	    {
		// There IS a line splice — consume it and peek the real char
		_ss.get();
		if ( next == '\r' && _ss.peek() == '\n' )
		    _ss.get();
		++_lf; _column = 0;
		ch = _ss.peek();
		_pos = _ss.tellg();
		return ch;
	    }
	    // Not a line splice — rewind
	    _ss.seekg(saved);
	}
	return ch;
    }
    bool getline(std::string &s)
    {
	int ch;
	s.clear();
	while ( _ss.good() && !_ss.eof() && (ch=_ss.get()) != -1 && ch != '\r' && ch != '\n' )
	    s += ch;
	if ( ch == -1 ) { return !s.empty(); }
	/**/ if ( ch == '\n' ) { ++_lf; _column = 0; _pos = _ss.tellg(); }
	else if ( ch == '\r' ) { ++_cr; _column = 0; _pos = _ss.tellg(); }
	else { ++_column; }
	if ( _ss.peek() == '\n' )
	{
	    _ss.get();
	    ++_lf;
	    _pos = _ss.tellg();
	}
	return !s.empty();
    }
    void setpos(int row, int col) { _lf = _cr = (row-1); _column = col; }
    void showerror(int row=0, int col=0);
};

// very simple exception container
class Exception: public std::exception
{
protected:
    std::string _msg;
public:
    explicit Exception(const std::string& message): _msg(message) {}
    virtual ~Exception() throw() {}
    virtual const char *what() const throw () { return _msg.c_str(); }
};

// streambuf class to throw an exception at sync
class throwbuf: public std::stringbuf
{
protected:
    TokenBase *_tb;
    Source *_src;
public:
    throwbuf() : std::stringbuf() { _tb = NULL; _src = NULL; }
    virtual int sync();
    TokenBase *token() { return _tb; }
    TokenBase *token(TokenBase *t) { return (_tb=t); }
    Source *source() { return _src; }
    Source *source(Source *s) { return (_src=s); }
};

class throwstream: public std::ostream
{
protected:
    throwbuf _tbuf;
public:
    throwstream() : std::ostream(&_tbuf) { exceptions(std::ios_base::badbit); }
    TokenBase *token() { return _tbuf.token(); };
    Source *source() { return _tbuf.source(); }
    Source *source(Source *s) { return _tbuf.source(s); }
    Source *source(Source &s) { return _tbuf.source(&s); }
    std::string str() const { return _tbuf.str(); }
    // Reset state + buffer at the START of each error. The stream has
    // exceptions(badbit); a prior `Throw(..) << .. << flush` leaves badbit set
    // after it throws, so WITHOUT this clear() the next `Throw(t) << "msg"`
    // re-throws ios_failure AT THE `<<` (before the message is built) — turning
    // every error after the first into a spurious "basic_ios::clear: iostream
    // error" and discarding the real message. str("") drops any stale message.
    throwstream& operator()(TokenBase *t)
    { clear(); _tbuf.str(std::string()); _tbuf.token(t); return *this; }
};


// Classification of a declared C++ class member for Itanium symbol emission.
enum class CppSymKind { Method, Ctor, Dtor };

// A parsed parameter signature (resolved base type + ref/const/pointer-depth),
// used when matching an upcoming parameter list against candidate overloads.
struct ParsedParamSig
{
    DataDef *base;
    bool is_ref;
    bool is_const;
    int pointer_depth;
    ParsedParamSig() : base(NULL), is_ref(false), is_const(false), pointer_depth(0) {}
};

// program class, keep things somewhat contained
class Program
{
public:
    struct FunctionRegistrationSpec
    {
	std::string id;
	datatype_vec_t params;
	fVOIDFUNC extfunc;
	bool is_method;
    };

    struct BuiltinRegistry
    {
	bool defaults_loaded = false;
	std::vector<FunctionRegistrationSpec> core_functions;
	std::vector<FunctionRegistrationSpec> process_functions;
	std::vector<FunctionRegistrationSpec> dlfcn_functions;

	void add_core_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method=false);
	void add_process_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method=false);
	void add_dlfcn_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method=false);
    };

    typedef void (*namespace_init_fn_t)(Program &);

    struct NamespaceRegistrationSpec
    {
	std::string name;
	namespace_init_fn_t init;
    };

    struct NamespaceRegistry
    {
	bool defaults_loaded = false;
	std::vector<NamespaceRegistrationSpec> specs;

	void add_namespace(const std::string &name, namespace_init_fn_t init);
    };

    struct RegistrationPolicy
    {
	struct RuntimeEvalChildPolicy
	{
	    bool enable_core_functions = true;
	    bool enable_process_functions = true;
	    bool enable_dlfcn_functions = true;
	    bool enable_std_namespace = true;
	    bool enable_madc_namespace = true;
	    bool enable_php_namespace = true;
	    bool enable_perl_namespace = true;
	    bool enable_python_namespace = true;
	    bool enable_ruby_namespace = true;
	    bool enable_js_namespace = true;
	    bool enable_rust_namespace = true;
	    bool restrict_headers_to_allowlist = false;
	    bool restrict_dlfcn_symbols_to_allowlist = false;
	    std::vector<std::string> allowed_headers;
	    std::vector<std::string> allowed_dlfcn_symbols;
	};

	bool enable_core_functions = true;
	bool enable_process_functions = true;
	bool enable_dlfcn_functions = true;
	// When false, a #load directive does not dlopen its named library; the
	// namespace is bound to the global symbol scope instead, so the symbols
	// must be provided explicitly (e.g. via `madc -l<lib>` or the host). Lets
	// a build make all linking explicit. (enable_dlfcn_functions=false is the
	// stricter sandbox knob that forbids #load outright.)
	bool enable_auto_library_loading = true;
	bool enable_runtime_eval_source_scope_access = true;
	bool enable_runtime_eval_expression_scope_access = true;
	bool enable_std_namespace = true;
	bool enable_madc_namespace = true;
	bool enable_php_namespace = true;
	bool enable_perl_namespace = true;
	bool enable_python_namespace = true;
	bool enable_ruby_namespace = true;
	bool enable_js_namespace = true;
	bool enable_rust_namespace = true;
	bool restrict_headers_to_allowlist = false;
	bool restrict_dlfcn_symbols_to_allowlist = false;
	// Header partition (madc-header-partition-handoff.md): when true, an
	// embedded baked-in header is bypassed in favor of the REAL system header
	// IFF it is a system-library shim (glibc/libstdc++ twin). madc-own headers
	// (no real twin: ns_php/__madc__/…) and compiler-owned freestanding headers
	// (stddef/limits/float, resolved in the gcc-internal dir) stay embedded.
	// This is the incremental shim-retirement lever — bypass system shims while
	// keeping madc's own headers. Data-driven: see embedded_header_is_system_library_shim().
	bool bypass_system_library_headers = false;
	std::vector<std::string> allowed_headers;
	std::vector<std::string> allowed_dlfcn_symbols;
	RuntimeEvalChildPolicy runtime_eval_source_policy;
    };

    struct ErrorInfo
    {
	bool has_error = false;
	std::string message;
	std::string file;
	int line = 0;
	int column = 0;
    };

    enum class DiagnosticSeverity
    {
	warning,
	error
    };

    enum class DiagnosticPhase
    {
	unknown,
	lexer,
	parser,
	compiler,
	runtime
    };

    struct Diagnostic
    {
	DiagnosticSeverity severity = DiagnosticSeverity::error;
	DiagnosticPhase phase = DiagnosticPhase::unknown;
	std::string message;
	std::string file;
	int line = 0;
	int column = 0;
    };

protected:
    TokenBase *_getToken();
    TokenBase *skipConditionalBlock();
    bool evaluateIfCondition();
    void popOperator(std::stack<TokenBase *> &, std::stack<TokenBase *> &);
//  inline int get(std::istream &is) { ++_column; return is.get(); }
    // initializers / finalizers
    void _tokenizer_init();
    void _parser_init();
    void _compiler_init();
    bool _compiler_finalize();
    // protected members
    int _braces;
//  std::streampos _pos;
    TokenBase *_prv_token;
    TokenBase *_cur_token;
    Source source;
public:
    MadcEngine *engine;
    Program *runtime_scope_prev;
    std::istream *input_stream;
    std::ostream *output_stream;
    std::ostream *error_stream;
    const madc::value *expression_context_root;
    RegistrationPolicy registration_policy;
    BuiltinRegistry builtin_registry;
    NamespaceRegistry namespace_registry;
    ErrorInfo last_error;
    std::vector<Diagnostic> diagnostics;
    keyword_map_t  keyword_map;		// reserved keywords
    // C++ alternative-token operators (and/or/not/bitand/...): word spellings
    // that are exact synonyms for symbolic operators ([lex.digraph]). Held
    // separately from keyword_map because the operator tokens are not
    // TokenKeyword subclasses. Populated (C++-gated) in add_keywords; looked up
    // and cloned in _getToken alongside keyword_map.
    std::map<std::string, TokenBase *> cpp_operator_map;
    datatype_map_t datatype_map;	// TokenDataType map
    datadef_map_t  datadef_map;		// data definitions defined by typedef or class
    datadef_map_t  struct_map;		// data definitions defined by struct
    // Type table identity layer — project segment (madc_typeid.h; design
    // docs/plans/2026-06-12-type-table-value-abi-design.md §2). Holds every
    // non-primitive DataDef this Program has been asked an id for; index i
    // <=> typeid MADC_TYPEID_PROJECT_BASE + i. Lazy registration order is
    // ask order (deterministic per compilation). Pointers, NOT values:
    // DataDef is polymorphic and ids must survive growth.
    std::vector<DataDef *> project_types;
    uint32_t type_id_for(DataDef *dd);	// THE lazy-stamp chokepoint
    DataDef *type_from_id(uint32_t id);	// segment-dispatching reverse lookup
    // Captured `template<typename T> class Name {...}` definitions for
    // Borland-model instantiation: name -> {type params, the class-body token
    // range}. `Name<ConcreteT>` clones+substitutes+re-parses it as a concrete
    // class. See docs/plans/2026-05-30-template-instantiation.md.
	struct TemplateDef {
	    std::vector<std::string> typeparams;   // e.g. ["T"]
	    std::vector<std::vector<TokenBase *>> typeparam_defaults;
	    std::vector<bool> typeparam_is_type;
	    std::vector<bool> typeparam_is_pack;
	    bool has_non_type_params;
	    std::string class_name;                // e.g. "Box"
	    std::vector<TokenBase *> body;         // cloned tokens: `class Name { ... }`
	std::string defining_namespace;        // current_namespace at capture (e.g. "std")
	DataDefCLASS *owner_class;             // enclosing class for member templates
	bool is_partial_specialization;        // template<class T> struct X<T*> {...}
	// For a partial spec: the pattern token sequence per arg slot (e.g. ["T","*"]
	// for `X<T*>`). Empty for a primary template.
	std::vector<std::vector<TokenBase *>> spec_pattern;
	TemplateDef() : has_non_type_params(false), owner_class(nullptr),
			is_partial_specialization(false) {}
    };
    // Templates are keyed by BARE name, but a same-named class template may be
    // declared in more than one namespace (e.g. std::char_traits and
    // __gnu_cxx::char_traits). Each bare name therefore maps to a vector of
    // per-namespace variants, disambiguated by TemplateDef::defining_namespace.
    // All selection goes through find_template() so the no-collision case keeps
    // exactly the old single-entry behavior.
    std::map<std::string, std::vector<TemplateDef>> template_map; // name -> variants
    // Select a template variant. owner_hint scopes nested member templates
    // (e.g. allocator<T>::rebind<U>); NULL selects namespace/global templates.
    // ns_hint != "" => exact defining_namespace match (or NULL); ns_hint == ""
    // => the sole matching-owner variant if unique, else prefer current_namespace,
    // then the global ("") variant, then the first matching-owner variant.
    TemplateDef *find_template(const std::string &name,
			       const std::string &ns_hint = std::string(),
			       DataDefCLASS *owner_hint = NULL);
    // The variant carrying a parsed body, if any (for completion gating).
    TemplateDef *template_with_body(const std::string &name);
    // Replace the same-namespace variant (merging template-default args from the
    // prior one) or append a new variant. only_if_absent => leave an existing
    // same-namespace variant untouched (first-wins, for bodyless forward decls).
    void register_template(const TemplateDef &td, bool only_if_absent);
    // Partial specializations (template<class T> struct X<T*> {...}), keyed by bare
    // class name. Kept OUT of template_map (its same-namespace merge would clobber
    // the primary). Selected at instantiation by most-specialized pattern unification.
    std::map<std::string, std::vector<TemplateDef>> partial_spec_map;
    // Choose the most-specialized partial spec of `name` whose pattern unifies with
    // the concrete arguments. Type slots deduce into out_subst; non-type slots must
    // fold to the same constant value. Returns NULL to fall back to the primary.
    TemplateDef *match_partial_specialization(const std::string &name,
	    const std::vector<TokenDataType *> &arg_types_by_slot,
	    const std::vector<std::string> &arg_spellings,
	    const std::vector<std::vector<TokenBase *>> &arg_tokens_by_slot,
	    const std::vector<bool> &param_is_type,
	    std::map<std::string, TokenDataType *> &out_subst,
	    std::map<std::string, std::string> &out_template_subst,
	    std::map<std::string, std::vector<std::string> > &out_pack_subst);
    // Unify a nested template-id pattern arg (e.g. `allocator<_Tp>`) against a
    // concrete type spelling (e.g. `std::allocator<char>`), deducing the spec's
    // type params. Fallback used by match_partial_specialization when the flat
    // unifier cannot match a template-id-shaped pattern slot.
    bool unify_nested_spec_pattern_arg(const std::string &pat_spelling,
	    const std::vector<std::string> &spec_params,
	    const std::string &concrete_spelling,
	    std::map<std::string, DataDef *> &ded, int &score,
	    std::map<std::string, std::string> *out_tmpl = NULL,
	    std::map<std::string, std::vector<std::string> > *out_pack = NULL);
    // Evaluate a `__void_t<Args...>` detection-idiom partial-spec slot: matches a
    // concrete void IFF every Arg (a `typename PARAM::member` dependent type) resolves
    // after substituting the already-deduced params. The SFINAE half of the std
    // detection idiom (__detected_or / __iterator_traits / allocator_traits).
    bool eval_void_t_detection_slot(const std::string &slot_spelling,
	    const std::string &concrete_spelling,
	    const std::map<std::string, DataDef *> &ded, int &score);
    // Confirm that a dependent member chain `BASE::seg1::seg2::...` (where some
    // seg names a TEMPLATE member, e.g. `rebind<_Up>`) resolves to a real type —
    // the part of the __void_t SFINAE probe the plain type-alias walk cannot do.
    // Builds the qualified-id tokens with deduced params substituted and resolves
    // via resolve_typename_type_token in an isolated stream. True iff it resolves.
    bool confirm_dependent_member_type(DataDef *base,
	    const std::vector<std::string> &segs,
	    const std::map<std::string, DataDef *> &ded);
    // Constant-fold ONE non-type template argument from its already-collected
    // token vector (+ spelling), WITHOUT touching the live token stream or
    // instantiating anything — a LOCAL cursor only. Recognizes manifest integer/
    // bool literals and the type-trait builtins (`__has_trivial_destructor(T)`,
    // `__is_same(A,B)`, …). Returns false (→ keep the spelling) for any form it
    // cannot fold with certainty (dependent / value-dependent / unsupported).
    bool fold_nontype_template_arg(const std::vector<TokenBase *> &argtoks,
				   const std::string &spelling, int64_t &out);
    // Evaluate a NON-dependent non-type template argument (a `<...>` slot that is
    // an integral/bool constant expression — `(1==1)`, `is_trivial<int>::value`,
    // `__is_bitwise_relocatable<T>::value` for concrete T) to its value via the
    // SAME constant-expression evaluator `static_assert` uses, parsing a clone of
    // the collected arg tokens in an ISOLATED stream. Returns false (keep the raw
    // tokens) for anything still dependent / unfoldable. This is the substitution-
    // time non-type-arg fold (g++ convert_nontype_argument / clang converted-
    // constant-expression) without which `enable_if<C,T>` never selects its
    // partial spec unless C is a literal.
    bool fold_nontype_arg_constant(const std::vector<TokenBase *> &argtoks,
				   int64_t &out);
    // The canonical, identifier-safe instantiation-key fragment for ONE template
    // argument: a foldable non-type arg renders as its normalized VALUE (so
    // `<true>`, `<1>`, and `<__has_trivial_destructor(int)>` all key identically
    // and select the matching specialization), else the sanitized spelling
    // (byte-identical to the legacy behavior). EVERY template-instantiation
    // key-build site routes through this so keys agree corpus-wide — the
    // canonical-forms discipline (docs/plans/2026-06-13-canonical-forms-on-mc11ir-sketch.md);
    // canonicalizing at only SOME sites shatters identity (the 202-regression).
    std::string canonical_arg_key_fragment(const std::vector<TokenBase *> &argtoks,
					   const std::string &spelling);
	struct TemplateAliasDef {
	    std::vector<std::string> typeparams;
	    std::vector<std::vector<TokenBase *>> typeparam_defaults;
	    std::vector<bool> typeparam_is_type;
	    std::vector<bool> typeparam_is_pack;
	    bool has_non_type_params;
	    std::string alias_name;
	std::vector<TokenBase *> target;
	std::string defining_namespace;
	DataDefCLASS *owner_class;
	TemplateAliasDef() : has_non_type_params(false), owner_class(nullptr) {}
    };
    std::map<std::string, std::vector<TemplateAliasDef>> template_alias_map;
    // C++14 VARIABLE TEMPLATE: `template<...> [inline constexpr] T name = init;`
    // (std::numbers::e_v, pi_v, …). madc does not model these as first-class
    // values; it registers the name + typeparams + initializer tokens so a use
    // `name<Arg>` resolves to its arg-substituted initializer expression (parsed
    // inline at the use site). Keyed by simple name AND `ns::name`.
    struct VarTemplateDef {
	std::vector<std::string> typeparams;
	std::vector<TokenBase *> init;          // tokens after '=' up to ';'
	std::string defining_namespace;
    };
    std::map<std::string, VarTemplateDef> var_template_map;
    // C++20 CONCEPT: `template<...> concept Name = <constraint-expr>;`. madc does
    // not yet EVALUATE concept satisfaction, but it captures the constraint tokens
    // (+ typeparams) here so a future evaluator can decide `Name<Args>` by
    // substituting Args and evaluating the constraint (nested concept-ids,
    // type-trait `::value`, `derived_from`, requires-expression well-formedness).
    // Until the evaluator lands this is storage-only (no behavior change). Keyed by
    // simple name AND `ns::name`.
    struct ConceptDef {
	std::vector<std::string> typeparams;
	std::vector<TokenBase *> constraint;    // tokens after '=' up to ';'
	std::string defining_namespace;
    };
    std::map<std::string, ConceptDef> concept_map;
    std::vector<TokenBase *> last_skipped_template_decl;
    // The SOURCE name of the tracked free-function overload parseDeclaration
    // is about to hand to parseFunction ("operator<"), stamped on the FuncDef
    // BEFORE its body parses (the hidden-friend access grant compares
    // display names mid-body). Cleared after the parseFunction call.
    std::string pending_function_display_name;
    std::vector<std::string> last_skipped_template_typeparams;
    // Pack-ness of last_skipped_template_typeparams (parallel vector), so a
    // skipped member template's variadic typeparam (`typename... _Args`) is
    // preserved through register_skipped_class_template_function.
    std::vector<bool> last_skipped_template_typeparam_is_pack;
    // W2 (retire-std-hardcoding-design): non-member operator overload candidates
    // declared at namespace scope (e.g. std::operator<<). Member-operator
    // resolution already exists (class_operator_call); these let `obj << x`
    // ALSO consider a free `operator<<(ostream&, const char*)` and bind the
    // winner mangled-direct via itanium_mangle_std_free_template. Captured from
    // the declaration (signature as spellings + the ordered template params)
    // so deduction + mangling are data-driven, never a stream-specific picker.
    struct FreeOperatorOverload {
	std::string ns;                            // "std"
	std::string opname;                        // "operator<<"
	std::vector<std::string> template_params;  // ordered: $T0,$T1,…
	std::string return_spelling;               // "basic_ostream<char,_Traits>&"
	std::vector<std::string> param_spellings;  // {"basic_ostream<char,_Traits>&","const char*"}
    };
    std::vector<FreeOperatorOverload> free_operator_overloads;
    // Named (non-operator) free-function template overloads captured from REAL
    // system headers (libstdc++), e.g. std::getline. Reuses FreeOperatorOverload
    // (opname holds the function name). The call site (cir_builder) selects an
    // overload by arity + deduces template args from the call's arg types, then
    // binds MANGLED-DIRECT to the real libstdc++ Itanium symbol — NOT the
    // __ns_<ns>_<name> wrapper path (which is correct only for madc's own polyglot
    // namespaces). See itanium_mangle_std_free_template + project_cpp_mangled_direct.
    std::vector<FreeOperatorOverload> free_function_overloads;
    // NON-template C++ namespace free-function overload sets (e.g. the nine
    // inline `std::to_string(...)` definitions in <bits/basic_string.h>).
    // Each overload parses into its OWN Variable/FuncDef under a unique
    // internal symbol (unique_overload_symbol); this registry — keyed
    // "ns::name" — lets the call site enumerate and rank them by arg types
    // (the same generic score_arg_to_param ranking methods use).
    // param_spelling is the normalized source spelling of the parameter list,
    // used to tell a re-declaration of the SAME overload (reuse its Variable)
    // from a NEW overload (mint a fresh symbol).
    struct NamespaceFnOverload {
	std::string param_spelling;
	Variable *var;
    };
    std::map<std::string, std::vector<NamespaceFnOverload>> namespace_fn_overload_sets;
    // Rank ns::name's parsed overloads against the call's arg types; returns
    // the winning Variable, or NULL when no set (or no viable overload) exists.
    // `zero_args` (parallel to argtypes, optional) marks literal-0 arguments —
    // the null-pointer-constant rule ([conv.ptr]) must survive the re-rank
    // (global ::operator== sets rank here too: testfreeop's `a == 0`).
    Variable *find_namespace_function_overload(const std::string &ns,
					       const std::string &name,
					       const std::vector<const DataDef *> &argtypes,
					       const std::vector<bool> *zero_args = NULL);
    // A parsed CONCRETE free-operator function viable for the operand types:
    // ranks the union of every "::"+opname-suffixed overload set (all
    // namespaces + the global "" key). NULL when none binds. `zero_args`
    // (optional, index-aligned) marks integer-literal-zero arguments
    // (null-pointer constants).
    Variable *find_free_operator_function(const std::string &opname,
					  const std::vector<const DataDef *> &argtypes,
					  const std::vector<bool> *zero_args = NULL);
    // The DataDef an operand denotes for free-operator overload ranking
    // (reference-transparent via operand_object_class).
    DataDef *free_operator_arg_datadef(TokenBase *operand);
    // Mid-parse arity-check deferral: true when the call's namespace overload
    // set has a member accepting more than argc args (or a retained fn
    // template that could instantiate one) — the tail re-rank decides then.
    bool namespace_overload_set_accepts_more(TokenCallFunc *tc, size_t argc);
    // Namespace FUNCTION templates with retained declaration tokens (the
    // post-`template<...>` range: [specifiers] RET name(params) [noexcept]
    // { body }), keyed "ns::name". Bodies are not parsed at capture; a call
    // whose callee is the body-less placeholder instantiates on demand
    // (Borland monomorphize: substitute the deduced type args into the
    // retained tokens and re-parse as a concrete namespace function — it
    // registers in namespace_fn_overload_sets and call_target_funcdef ranks
    // it). Tokens are retained raw (the program owns its token stream; same
    // lifetime model as last_skipped_template_decl) and cloned per
    // instantiation.
    struct FnTemplateDef {
	std::vector<std::string> typeparams;
	std::vector<std::vector<TokenBase *>> typeparam_defaults;
	std::vector<bool> typeparam_is_type;
	std::vector<bool> typeparam_is_pack;
	std::vector<TokenBase *> decl;
	std::string ns;
	// Owner class for a MEMBER function template being instantiated: pushed
	// on class_scope_stack during the body parse so the params/body resolve
	// class-scope members ([basic.scope.class]). NULL for free/namespace fn
	// templates. (No default initializer — keeps the aggregate-init shape.)
	DataDefCLASS *owner_class;
	// INSTANCE (non-static) member function template: the instantiated body
	// is parsed AS A METHOD of owner_class (hidden `__this`, member access),
	// not as a free function — the clang/gcc model (a member fn template is
	// instantiated as a CXXMethodDecl of the class; static-ness is just a
	// flag). False for free/namespace templates AND for STATIC member
	// templates (those stay on the free-function parse).
	bool instance_method;
	FnTemplateDef() : typeparams(), typeparam_defaults(), typeparam_is_type(),
	    typeparam_is_pack(), decl(), ns(), owner_class(NULL),
	    instance_method(false) {}
    };
    std::map<std::string, std::vector<FnTemplateDef>> fn_template_map;
    // BODY-LESS free/namespace function template declarations (no `{ body }` to
    // instantiate — e.g. `template<class T> T declval();`), keyed "ns::name".
    // Kept OUT of fn_template_map (which drives body instantiation + the arity-
    // deferral signal) so instantiation behavior is unchanged; read ONLY to
    // form a call's return TYPE by substituting explicit template args into the
    // declared return (resolve_namespace_fn_template_call_return_type — the
    // clang deduction-forms-the-function-type-without-a-body model).
    std::map<std::string, std::vector<FnTemplateDef>> fn_template_decl_map;
    std::set<std::string> fn_template_instantiated;   // "ns::name<t1,t2,...>" memo
    // inst_key -> the overload Variable that instantiation registered, so an
    // operator USE site can call the instantiated definition directly.
    std::map<std::string, Variable *> fn_template_instantiated_vars;
    // > 0 while re-parsing an instantiated function-template body (nesting =
    // instantiation triggering instantiation). static_asserts inside such a
    // body are consumed unevaluated, exactly like instantiated class-template
    // member bodies (parse_static_assert_statement).
    size_t fn_template_instantiation_depth = 0;
    // True only while resolving a qualified member-TYPE chain (`typename X<..>::type`):
    // a concrete-arg trailing-type-pack class template is then REALLY instantiated
    // (body parse + member eval) so its member types fold (e.g. `__construct_helper<
    // _Tp,_Args...>::type` for the vector<T*> SFINAE trait). Elsewhere — notably the
    // constant-fold path (`__and_<..>::value`) — variadic templates stay opaque, as a
    // recursive variadic body (`__and_`, `tuple`) would otherwise re-instantiate
    // unboundedly. Scoped-set around the member-type instantiate_template_id calls.
    bool allow_variadic_real_inst = false;
    // STICKY variant of the above: stays on through a real-instantiation SUBTREE
    // whose root template forwards its meaning through a dependent-member base
    // (`__invoke_result : __result_of_impl<...>::type`). Unlike allow_variadic_real_inst
    // (consumed once at each instantiate_template_use entry), this propagates into
    // nested arg-resolution + base re-parse so the whole forwarding chain
    // (__is_invocable -> __invoke_result -> __result_of_impl) really instantiates.
    // Bounded by variadic_inst_in_progress so a recursive body cannot re-enter.
    bool variadic_real_inst_sticky = false;
    // Class-template real-instantiations in flight, keyed by mangled name. Re-entering
    // the same key while variadic_real_inst_sticky is on returns the opaque placeholder
    // instead of recursing — the recursion bound for the sticky forwarding-trait path.
    std::set<std::string> variadic_inst_in_progress;
    // Alias-template uses currently being resolved (keyed tname + arg spellings).
    // Re-entering the SAME key is a resolution CYCLE (a self-referential trait, now
    // reachable because variadic members really instantiate) — it short-circuits to
    // the opaque placeholder instead of recursing into a stack overflow. A genuine
    // (acyclic) deep nest uses distinct keys and resolves normally.
    std::set<std::string> alias_resolve_in_progress;
    // Member-fn-template instantiations currently in flight, keyed by LOGICAL identity
    // (owner + fn name + arg-type spellings), NOT call site. Breaks the allocator
    // trait cycle construct -> _S_construct -> __has_construct -> __test -> construct:
    // re-entering the same logical instantiation returns early (the body completes in
    // the outer frame) instead of recursing into a stack overflow. Call-site keying
    // (tc->var.name) can't catch this — each cycle hop is a distinct call site.
    std::set<std::string> member_fn_inst_in_progress;
    // >0 while parsing the operand of an UNEVALUATED context (decltype(...),
    // sizeof, noexcept). C++ does not ODR-use entities named in an unevaluated
    // operand ([basic.def.odr]) — so a function-template call there forms only a
    // signature (its return type, via the resolver), never an instantiated body.
    // declval is the canonical case: libstdc++ DEFINES it (a static_assert body
    // "declval() must not be used!" that calls the declaration-only __declval) but
    // it is only ever named in unevaluated operands, so its body is never emitted.
    // Instantiating it would import the undefined __declval. Gate body
    // instantiation on this depth == 0.
    int unevaluated_operand_depth = 0;
    void instantiate_namespace_fn_template_for_call(TokenCallFunc *tc);
    // A STATIC member function template of a madc-LOCAL class (a monomorphized
    // template instance such as `_Destroy_aux<true>`) is registered
    // declaration-only with its body retained on the FuncDef
    // (register_skipped_class_template_function). When such a callee is called,
    // deduce its template params from the call's args, instantiate the retained
    // body via the shared free-fn-template machinery, and alias the instantiated
    // definition's emitted symbol (local_emit_name) to the call's symbol so the
    // call's extern resolves to the real definition at link. libstdc++-EXPORTED
    // member templates keep the mangled-direct path (member_template_method_call).
    void instantiate_member_fn_template_for_call(TokenCallFunc *tc);
    // Resolve the CONCRETE return type of a member-function-template call by
    // substituting the call's explicit/deduced template args into the retained
    // DEPENDENT return-type tokens (FuncDef::member_template_return_tokens) and
    // resolving — WITHOUT instantiating a body (the clang SubstDecl model).
    // Returns the concrete DataDef, or NULL on substitution failure (SFINAE) /
    // missing data. Used for decltype/unevaluated calls to body-less member
    // templates whose normal (body) instantiation produced no __mti definition.
    DataDef *resolve_member_template_call_return_type(class FuncDef *fd,
		const std::vector<DataDef *> &explicit_targs);
    // Deduce + instantiate a retained free-OPERATOR template body for
    // `lhs <op> rhs` (e.g. operator+(const basic_string&, const _CharT*) —
    // libstdc++ does not export that shape, so the BODY must compile).
    // Returns the deduced return class; *callee_out = the registered overload.
    DataDef *instantiate_free_operator_template(const std::string &opname,
						TokenBase *lhs, TokenBase *rhs,
						Variable **callee_out);
    // Cfront lowering: when a binary class-operand operator resolves ONLY via
    // a retained operator template (no member operator, no exported free
    // shape), return a call node to the instantiated overload. NULL = not
    // this path; the normal operator machinery proceeds.
    TokenBase *lower_free_operator_to_call(class TokenOperator *to,
					   bool no_rewrite = false);
    // C++20 rewritten candidates ([over.match.oper]): != via ==, reversed
    // ==, relationals via (x <=> y) @ 0. Consulted only after every direct
    // candidate set missed.
    TokenBase *rewritten_operator_candidate(class TokenOperator *to);
    // madc strict-equality === domain rule: lower `x === y` to `x == y`.
    TokenBase *lower_strict_equality_domain_rule(class TokenOperator *to);
    // The std comparison-category class a builtin <=> yields
    // ([expr.spaceship]); NULL when <compare> has not been parsed.
    DataDef *comparison_category_class(class TokenOperator *to);
    // Namespaces named by `using namespace X;` directives (C++
    // [namespace.udir]). The single-Variable import model skips a member
    // whose name a global already claimed; unqualified CALL resolution
    // consults these to bind the namespace overload when the global's arity
    // rejects the call (using_namespace_call_fallback).
    std::vector<std::string> active_using_namespaces;
    Variable *using_namespace_call_fallback(Variable *var, size_t argc);
    // Parse an explicit template-argument list after a resolved function name
    // (`name<long, int>(...)`) into concrete DataDefs, consuming through the
    // closing '>'. Bails to opaque consumption (skip_template_id_suffix
    // semantics) and returns an EMPTY list when an argument is not a
    // resolvable type, so a value argument never mis-binds a type parameter.
    std::vector<DataDef *> capture_call_template_args();
    // Record (then push back) the upcoming balanced parameter-list tokens —
    // the parser sits just after the function declarator's '(' — returning a
    // normalized spelling used as the overload-identity key.
    std::string peek_param_list_spelling();
    struct PendingTemplateInstantiation {
	std::string mangled_name;
	std::string canonical_spelling;
	std::vector<TokenDataType *> args;
    };
    std::map<std::string, std::vector<PendingTemplateInstantiation>> pending_template_instantiations;
    std::set<std::string> template_completion_requested;    // mangled template aliases that should be completed when body appears
    std::set<std::string> template_instantiated;           // mangled names done
    std::vector<DataDefCLASS *> class_scope_stack;	// active C++ class scopes for nested type lookup
    std::map<DataDef*, DataDefPTR*> ptr_type_cache; // cached pointer-to-T DataDefs
    std::map<DataDef*, DataDefREF*> ref_type_cache; // cached reference-to-T DataDefs (alias-spelled T&)
    funcdef_map_t  funcdef_map;		// function definitions
    variable_map_t literal_map;		// string literals
    namespace_map_t namespace_map;	// namespace registries (std::, etc.)
    namespace_datatype_map_t namespace_datatype_map; // namespace-owned type names
    std::map<std::string, std::vector<std::string>> inline_namespace_children;
    // Lexical namespace context. Each entry is the FULL active namespace
    // ("std::__cxx11"); back() is the active one (empty stack = global scope).
    // The idiomatic twin of class_scope_stack: a vector (not std::stack) so
    // the enclosing-chain walk stays possible. Mutate ONLY via NamespaceScope.
    std::vector<std::string> namespace_stack;
    const std::string &current_namespace() const
    {
	static const std::string global_scope;
	return namespace_stack.empty() ? global_scope : namespace_stack.back();
    }
    // RAII guard for namespace_stack — exception-safe by construction.
    class NamespaceScope
    {
	Program &pgm;
	NamespaceScope(const NamespaceScope &);
	NamespaceScope &operator=(const NamespaceScope &);
    public:
	NamespaceScope(Program &p, const std::string &ns) : pgm(p)
	{ pgm.namespace_stack.push_back(ns); }
	~NamespaceScope() { pgm.namespace_stack.pop_back(); }
    };
    // Canonical C++ spelling of the template-id being instantiated right now,
    // stashed by instantiate_template_use around the class re-parse so
    // TokenCLASS::parse can record it on the new DataDefCLASS for bodyless C++
    // method binding.
    std::string instantiating_canonical_spelling;
    bool instantiating_dependent_surface;
    bool parsing_defaulted_member_template_constructor;
    std::vector<std::string> namespace_preference; // ordered namespace lookup; "c" means normal lexical/global resolution
    std::map<std::string, void *> dlopen_map;	// dlopen handles for loaded libraries
    // function-like macro definitions: #define NAME(params) body
    struct MacroDef {
	std::vector<std::string> params;  // parameter names
	bool variadic = false;           // trailing ... / __VA_ARGS__
	std::string variadic_param;       // GNU named varargs parameter (`args...`)
	std::string body;                 // body template with param names as placeholders
    };
    std::map<std::string, MacroDef> macro_map;	// function-like macros
    enum LazyKind { lkVariable = 1, lkFunction = 2, lkType = 3, lkStruct = 4 };
    struct LazyEntry { int header; LazyKind kind; };
    std::map<std::string, LazyEntry> lazy_map;	// deferred symbol registration
    std::map<std::string, std::string> define_map;	// #define name value
    std::set<std::string> disabled_builtin_names;	// -fno-builtin-foo from CLI/tests
    std::map<std::string, std::stack<std::string>> _macro_save_stack; // #pragma push_macro / pop_macro
    std::vector<std::string> include_paths;	// -I include search paths (for #include "file.h")
    std::vector<std::pair<std::string,std::string>> cli_defines;	// -DNAME[=VALUE] command-line defines (applied after builtins)
    void add_include_dir(const std::string &dir);	// normalize (trailing '/') + append to include_paths
    void add_cli_define(const std::string &def);	// split NAME[=VALUE] (bare => "1") into cli_defines
    std::map<std::string, bool> included_files;	// #include files already tokenized (require_once semantics)
    // realpath -> detected include-guard macro ("" = guard-less, always
    // re-tokenize). Drives gcc's multiple-include optimization in
    // should_tokenize_include.
    std::map<std::string, std::string> include_guard_by_file;
    std::stack<bool> ifdef_stack;	// conditional compilation state stack
    std::stack<bool> ifdef_done_stack;	// tracks if any branch in #if/#elif/#else was taken
    std::queue<TokenBase *> ast;	// Abstract Syntax Tree
    std::deque<TokenBase *> tokens;	// parsed token queue
    std::deque<TokenBase *> injected_tokens; // synthetic lexer output for lowered directives
    // User-defined function AST nodes, in source order. Parallel to the
    // ast queue. Populated by parseFunction / parseLambda; consumed by
    // Program::compile in a pre-pass to create funcnodes (labels) before
    // globals compile, so global fn-pointer inits can LEA the target label.
    std::vector<TokenBase *> pending_funcs;
    struct DeferredFunctionBody {
	Variable *var;
	Method *method;
	std::vector<TokenBase *> body_tokens;
	std::vector<TokenBase *> definition_tokens;
	// A captured trailing return type (`-> T`) of an `auto`-return method,
	// resolved when the deferred body materializes (parameters back in scope).
	std::vector<TokenBase *> trailing_ret_tokens;
	// A constructor's mem-initializer-list tokens (after ':', before the
	// body '{'), parsed at class COMPLETION like the body — initializer
	// ARGUMENTS are complete-class context ([class.base.init]): real
	// _Vector_base's `: _M_impl(..., std::move(__x._M_impl))` names a
	// member declared after the constructor.
	std::vector<TokenBase *> ctor_init_tokens;
	bool full_definition;
	const char *file;
	int line;
	int column;
	DeferredFunctionBody() : var(NULL), method(NULL), full_definition(false), file(NULL), line(0), column(0) {}
    };
    std::vector<DeferredFunctionBody> *deferred_function_body_sink;
    // Mem-initializer tokens captured for the NEXT enqueue_deferred_function_body
    // (set by parseFunction's ':' arm when a class-body ctor defers).
    std::vector<TokenBase *> pending_deferred_ctor_inits;
    TokenBase *parse_ctor_initializer_list(FuncDef *func);
    // Lazy member-function-body instantiation ([temp.inst] conformance): for a
    // system-header class (typically a template instantiation), member-function
    // BODIES are NOT parsed at class-completion time — they are stashed here,
    // keyed by emit symbol (var->name), and parsed only when ODR-used. This
    // mirrors g++ (a class instantiation instantiates declarations, not member
    // definitions) and dissolves walls in unused inline bodies (e.g.
    // basic_string::_M_local_data). The cir_builder reachability fixpoint
    // materializes a deferred body the moment its symbol enters referenced_funcs.
    std::map<std::string, DeferredFunctionBody> deferred_lazy_bodies;
    // Out-of-line member DEFINITIONS of a class template
    // (`template<class T> RET ClassName<T>::member(params){body}` — the bits/*.tcc
    // shape, e.g. std::vector::_M_realloc_insert). A class instantiation re-parses
    // only the class BODY (in-class member defs); an out-of-line def is a separate
    // top-level template, so its body was never captured -> "import of undefined
    // item". Captured at parse time keyed by "<defining-ns>::<class-name>"; when
    // ClassName<Args> is monomorphized, each captured def is substituted with the
    // concrete args and registered as a full_definition deferred_lazy_body keyed by
    // the instantiated member's emit symbol, so the body materializes on ODR-use
    // ([temp.inst]p2) exactly like an in-class-defined member (lazy, not eager —
    // unused out-of-line members never instantiate).
    struct OutOfLineMemberDef {
	std::string member_name;
	std::vector<std::string> typeparams;	// CLASS (outer) type-params
	std::vector<TokenBase *> decl;	// full decl incl body, owned clones
	// An out-of-line member TEMPLATE (`template<class T> template<class U>
	// RET S<T>::f(U){body}`, two-level head — e.g. vector::_M_realloc_insert's
	// C++11 variadic form) attaches its body to the monomorphized member as a
	// member_template_decl, instantiated per-call by the sub-gap-5 path; a plain
	// member def becomes a deferred (ODR-use-lazy) full body. inner_typeparams /
	// inner_is_pack are the MEMBER (inner) template parameters.
	bool is_member_template = false;
	std::vector<std::string> inner_typeparams;
	std::vector<bool> inner_is_pack;
    };
    std::map<std::string, std::vector<OutOfLineMemberDef>> out_of_line_member_defs;
    void register_outofline_member_instantiations(
	const std::string &class_name, const std::string &defining_namespace,
	const std::string &registered_mangled, DataDefCLASS *ddc,
	const std::vector<TokenDataType *> &arg_types_by_slot,
	const std::vector<std::vector<TokenBase *> > &arg_tokens_by_slot);
    bool parsing_cpp_struct_class;
    // Set by TokenSTRUCT::parse when delegating a UNION with class-only syntax
    // to the class parser ([class.union]); TokenCLASS::parse consumes it and
    // marks the DataDefCLASS union_layout.
    bool parsing_cpp_union_class = false;
    // Statement-level qualified-call (`php::foo(args);`): the CALLEE namespace
    // override for HEAD resolution only ([basic.lookup] — the qualification
    // applies to the called name, not the arguments). Deliberately NOT part of
    // namespace_stack (it is a qualification override, not lexical scope):
    // active_cpp_lookup_namespace() consults it first, and parseCallFunc /
    // parseCallMethod clear it before the argument loop so arguments resolve
    // in the lexical namespace. Set/restored only via QualifiedCalleeScope.
    std::string stmt_callee_namespace;
    class QualifiedCalleeScope
    {
	Program &pgm;
	std::string saved;
	QualifiedCalleeScope(const QualifiedCalleeScope &);
	QualifiedCalleeScope &operator=(const QualifiedCalleeScope &);
    public:
	QualifiedCalleeScope(Program &p, const std::string &ns)
	    : pgm(p), saved(p.stmt_callee_namespace)
	{ pgm.stmt_callee_namespace = ns; }
	~QualifiedCalleeScope() { pgm.stmt_callee_namespace = saved; }
    };
    // When set, TokenCLASS::parse parses and registers the class/struct
    // DEFINITION only and returns at the closing '}', WITHOUT consuming a
    // trailing instance declarator. Used when a method-bearing struct is
    // nested inside another aggregate's body: the class parser registers the
    // nested type, then the enclosing struct-body parser claims the trailing
    // member declarator (`struct Inner { void f(){} } member;`).
    bool class_definition_only;
    // Source-ordered top-level declarations for CIR tree generation.
    // Each entry records what was declared and in what order, matching
    // the order c2m's parser produces in its MODULE LIST. The legacy
    // JIT/compile path ignores this; only the CIR backend consumes it.
    enum class DeclKind { dkTypedef, dkStruct, dkUnion, dkEnum, dkGlobalVar };
    struct TopDecl {
	DeclKind kind;
	std::string name;	// typedef alias, struct tag, or variable name
	DataDef *dd;		// the DataDef (struct, typedef target, etc.)
	TokenDataType *tdt;	// for typedefs: the TokenDataType entry
	Variable *var;		// for global vars: the Variable
	const char *file;
	int line;
	TokenBase *origin;	// per-occurrence source token (alias/tag/decl head); NULL → use file/line
	TokenDecl *decl;	// for global vars: the TokenDecl carrying initializer (initialize/init_list); NULL → no init
	bool struct_body;	// dkTypedef only: this combined `typedef struct Tag {...} Alias;` carries the tag's full body (it is the tag's definition point). A `typedef struct Tag *p;` referencing the tag is false.
	TopDecl() : kind(DeclKind::dkStruct), dd(nullptr), tdt(nullptr), var(nullptr), file(nullptr), line(0), origin(nullptr), decl(nullptr), struct_body(false) {}
    };
    std::vector<TopDecl> top_decls;
    // Host-callback registrations (libmadc register_function): the embedding
    // host exposes a native function to scripts. _parser_init declares each
    // as an ordinary prototype (add_host_callbacks), and the CIR builder
    // synthesizes a module trampoline definition
    //   RET name(params) { return __madc_host_cb_<k>([bound,] params...); }
    // whose import symbol the JIT session resolves to `entry` at MIR link.
    // Types are carried as Kind codes (not DataDef*) so registrations
    // survive Program re-creation across recompiles.
    struct HostCallbackReg {
	enum Kind { K_VOID = 0, K_BOOL, K_INT, K_REAL, K_CSTR };
	std::string name;	 // script-visible function name
	std::string import_sym;	 // __madc_host_cb_<k> the trampoline calls
	uintptr_t entry = 0;	 // host address bound to import_sym at link
	uintptr_t bound = 0;	 // deduced-form callback passed as hidden first arg (0 = none)
	int returns = K_VOID;
	std::vector<int> params;
    };
    std::vector<HostCallbackReg> host_callback_regs;
    // Names registered via a user `typedef` (populated when a typedef is
    // recorded). Used to decide when a type was written with a typedef
    // alias so CIR emits ID("alias"); distinguishes user typedefs from
    // builtin type keywords (whose token str can differ from the DataDef
    // name, e.g. `int` -> "int32_t", making a name compare unreliable).
    std::set<std::string> user_typedef_names;
    std::stack<TokenCpnd *> compounds;	// stack to manage nested brackets
    std::vector<TokenSWITCH *> switch_stack; // active switch parse contexts for nested case/default hoisting
    std::vector<TokenCASE *> switch_case_stack; // current active case/default while parsing each switch
    TokenProgram *tkProgram;		// program token
    TokenCpnd *tkFunction;		// function we are currently in
    int try_depth;			// >0 when compiling inside a try body
    std::string cur_func_name;		// name of current function being compiled (for diagnostics)
    throwstream Throw;			// throw an error
    int script_argc;			// argc for the .mad script
    char **script_argv;			// argv for the .mad script
    bool keep_trivia = false;		// full-fidelity: preserve whitespace/comments
					// as TokenBase::leading_trivia (IDE/.mc11);
					// off by default → zero cost for batch
    std::string _trailing_trivia;	// whitespace/comments after the last token
					// (full-fidelity; reconstruct_source appends it)
    bool _include_iostream;		// #include <iostream> was seen during tokenization
    bool _include_stdio;		// #include <stdio.h> was seen during tokenization
    bool _include_string;		// #include <string> was seen during tokenization
    // Intern file paths so TokenBase::file pointers stay stable for
    // the program's lifetime. Lexer used to store `c_str()` of a
    // stack-local std::string into tokens — the pointer dangled the
    // moment the include scope ended, leaving every later read of
    // tb->file undefined. NOTE: this must be its OWN store —
    // `included_files` (the #include guard map) is CLEARED by
    // _tokenizer_init() per tokenize, which dangled every previously
    // interned pointer (the header-line-under-main-file diagnostic
    // misattribution, and expression-unit user tokens whose file read
    // as reused-heap garbage).
    const char *intern_file(const std::string &s) {
	return interned_files.emplace(s).first->c_str();
    }
    std::set<std::string> interned_files;	// stable token file names (never cleared)

    enum class LinkageSpec { Cpp, C };
    LinkageSpec current_linkage = LinkageSpec::Cpp;
    bool parsing_extern_decl = false;	// current declaration originated from `extern`
    bool parsing_static_decl = false;	// current declaration originated from `static` (propagates through `static struct X x;` path so parseDeclaration knows to allocate persistent storage)
    bool parsing_const_decl = false;	// current declaration originated from `const` — set vfCONSTANT on the variable
    bool parsing_typedef_decl = false;	// propagates through `typedef const struct ...` path

    std::stack<int> _pack_stack;	// #pragma pack(push, N) / pop stack
    int pack_stack_top() { return _pack_stack.empty() ? 0 : _pack_stack.top(); }

    bool colors;
    enum LanguageStd {
	STD_MADC,	// default: C++ keywords reserved
	STD_C78, STD_C86, STD_C88, STD_C89, STD_C90, STD_C94, STD_C95, STD_C99, STD_C11, STD_C17, STD_C23,
	STD_CPP98, STD_CPP03, STD_CPP11, STD_CPP14, STD_CPP17, STD_CPP20, STD_CPP23, STD_CPP26
    } language_std;
    // The C++ level that plain madc (STD_MADC) — and any non-C++ selection that
    // still presents as C++ — targets: the single configurable "default bar",
    // NOT a literal scattered across the predefined-macro table + the
    // __cplusplus switch. It deliberately LAGS the highest enum value
    // (STD_CPP26): the enum is what a user MAY request via `--std=`; this is what
    // gets presented when nothing is requested. Raising the bar madc defaults to
    // is one assignment here (CLI / embedder can override). See
    // cplusplus_value_for_std(). The bar is gated low because every step up
    // forces madc to parse more of the C++20+ system-header surface
    // (ranges/concepts) — see docs/plans for the raise strategy.
    LanguageStd default_cpp_std;
    // GNU dialect modifier: `--std=gnuNN` / `--std=gnu++NN` selects the base
    // standard (language_std) WITHOUT strict-ANSI conformance — gcc's actual
    // default dialect is gnu17, not c17. Feature gating stays on
    // language_std; this flag only controls strictness presentation.
    bool gnu_dialect;
    bool is_c_mode() const { return language_std >= STD_C89 && language_std <= STD_C23; }
    bool is_cpp_mode() const { return language_std >= STD_CPP98 && language_std <= STD_CPP26; }
    // gcc parity for C modes: -std=cNN defines __STRICT_ANSI__, -std=gnuNN
    // (gcc's default dialect) does not — real glibc headers branch on it
    // (features.h suppresses _DEFAULT_SOURCE under strict ANSI, hiding
    // timercmp/strdup-class declarations SMAUG needs).
    // KNOWN DIVERGENCE: STD_MADC and the C++ modes keep the captured strict
    // presentation even though plain g++ (gnu++17) is non-strict — lifting
    // it opens glibc's `!__STRICT_ANSI__` float regions (__HAVE_FLOAT128 →
    // __float128/_FloatN declarations) that madc cannot type yet. Lift once
    // __float128/_FloatN land (the __int128 P0 track's float sibling).
    bool strict_ansi_mode() const
    { return !(is_c_mode() && gnu_dialect); }
    // The __STDC_VERSION__ value a C mode mandates (gcc parity; c89/c90
    // predate the macro — NULL = leave undefined, like gcc -std=c89).
    const char *stdc_version_for_std() const {
	switch ( language_std ) {
	case STD_C94: case STD_C95: return "199409L";
	case STD_C99: return "199901L";
	case STD_C11: return "201112L";
	case STD_C17: return "201710L";
	case STD_C23: return "202311L";
	default:      return (const char *)0;
	}
    }
    // K&R-era recovery (old-style parameter declarations, file-scope
    // implicit-int definitions) is admitted ONLY under an explicit C
    // standard that predates C23 (which removed them) — never in the
    // madc dialect or any C++ mode.
    bool knr_supported() const { return language_std >= STD_C78 && language_std < STD_C23; }
    // The madc dialect is a C++ superset: like every explicit C++ mode it
    // presents to system headers as g++ (__cplusplus/__GNUG__ defined), so
    // the REAL glibc/libstdc++ headers configure their C++ surface. Only
    // explicit C standards present as plain gcc.
    bool presents_as_cpp() const { return language_std == STD_MADC || is_cpp_mode(); }
    bool auto_includes_enabled() const { return language_std == STD_MADC; }
    // A C++ reserved keyword introduced in `min_std` is active iff we are in the
    // madc dialect (reserves the full C++ keyword set) or in an explicit C++ mode
    // at/after that standard. The C++ enumerators are contiguous and ordered
    // (CPP98 < CPP03 < ... < CPP26), so the `>=` compare is a pure version floor;
    // is_cpp_mode() excludes the (lower-valued) C enumerators. NEVER active in C.
    bool cpp_keyword_active(LanguageStd min_std) const
    { return language_std == STD_MADC || (is_cpp_mode() && language_std >= min_std); }
    // The __cplusplus value a given C++ LanguageStd mandates (C++26 uses g++'s
    // provisional 202400L until the standard fixes one).
    static const char *cplusplus_value_for(LanguageStd std) {
	switch ( std ) {
	case STD_CPP98: case STD_CPP03: return "199711L";
	case STD_CPP11: return "201103L";
	case STD_CPP14: return "201402L";
	case STD_CPP17: return "201703L";
	case STD_CPP20: return "202002L";
	case STD_CPP23: return "202302L";
	case STD_CPP26: return "202400L";
	default:        return "201703L";
	}
    }
    // The __cplusplus value the ACTIVE mode presents. An explicit C++ `--std=`
    // uses that level; STD_MADC (and any non-C++ selection) falls back to the
    // single configurable `default_cpp_std` bar — no magic literal here.
    const char *cplusplus_value_for_std() const {
	return cplusplus_value_for(is_cpp_mode() ? language_std : default_cpp_std);
    }
    bool set_language_standard(const std::string &standard);
    bool set_language_standard_option(const std::string &arg);
    bool aot_tracking;
    bool instrument_functions;
    bool skip_includes;		// --emit-function: lex without processing #include
    std::set<std::string> pending_auto_include_headers;
    std::set<std::string> pending_auto_include_identifiers;
    bool suppress_auto_include_scan;
    struct AotDataRef {
	uint32_t label_id;
	uintptr_t address;
	size_t data_offset;
	uint8_t imm_offset;
    };
    std::vector<AotDataRef> aot_data_refs;
    std::vector<char *> aot_string_constants;
    struct AotDiscoveredData {
	std::string name;
	void *address;
	size_t size;
	DataDef *type;
	uint32_t count;
    };
    std::vector<AotDiscoveredData> aot_discovered_data;
    std::map<uintptr_t, size_t> aot_discovered_data_index;
    struct AotDataRange {
	uintptr_t start;
	size_t size;
	size_t data_offset;
    };
    std::map<uintptr_t, size_t> aot_layout_offsets;
    std::vector<AotDataRange> aot_layout_ranges;
    std::map<uintptr_t, std::string> external_symbol_map;
    fVOIDFUNC root_fn;

    Program();
    explicit Program(MadcEngine *eng);
    void attach_engine(MadcEngine *eng);
    bool lookup_aot_data_offset(uintptr_t address, size_t &out_offset) const;
    size_t aot_variable_storage_size(const Variable *var) const;
    void record_aot_variable_data(Variable *var);
    void record_aot_data(const std::string &name, void *address, size_t size,
	DataDef *type = NULL, uint32_t count = 1);

    void add_keywords();
    void add_datatypes();
    void ensure_registration_config();
    void clear_diagnostics();
    void clear_error();
    std::istream &input();
    std::ostream &output();
    std::ostream &error();
    void add_diagnostic(DiagnosticSeverity severity, DiagnosticPhase phase,
	const std::string &message, const char *file=NULL, int line=0, int column=0);
    const Diagnostic *last_diagnostic() const;
    void report_warning(DiagnosticPhase phase, const std::string &message,
	const char *file=NULL, int line=0, int column=0);
    void report_error(DiagnosticPhase phase, const std::string &message,
	const char *file=NULL, int line=0, int column=0);
    void set_error(DiagnosticPhase phase, const std::string &message,
	const char *file=NULL, int line=0, int column=0);
    void set_error(const std::string &message, const char *file=NULL, int line=0, int column=0);
    const char *diagnostic_severity_name(DiagnosticSeverity severity) const;
    const char *diagnostic_phase_name(DiagnosticPhase phase) const;
    bool can_show_diagnostic_source(const Diagnostic &diag) const;
    void print_diagnostic(std::ostream &os, const Diagnostic &diag, const char *suffix=NULL);
    void print_last_diagnostic(std::ostream &os, const char *suffix=NULL);
    void populate_builtin_registry();
    bool is_builtin_disabled(const std::string &name) const;
    void populate_namespace_registry();
    void register_function_specs(const std::vector<FunctionRegistrationSpec> &specs);
    void register_namespace_specs();
    void add_core_functions();
    void add_process_functions();
    void add_dlfcn_functions();
    void add_functions();
    void add_globals();
    void add_host_callbacks();	// declares host_callback_regs prototypes (libmadc register_function)
    void add_iostream();	// populates lazy_map for cout, cin, cerr (via #include <iostream>)
    void add_stdio();		// placeholder for #include <stdio.h> registration
    Variable *lazy_resolve(const std::string &name);	// on-demand variable/function registration
    DataDef  *lazy_resolve_type(const std::string &name);	// on-demand type/struct registration
    void add_namespaces();
    void add_madc_namespace();
    bool is_namespace_registration_enabled(const std::string &name) const;
    bool is_dynamic_library_loading_enabled() const;
    bool is_auto_library_loading_enabled() const;
    bool is_dynamic_symbol_fallback_enabled() const;
    bool is_runtime_eval_source_scope_access_enabled() const;
    bool is_runtime_eval_expression_scope_access_enabled() const;
    bool is_embedded_header_allowed(const std::string &name) const;
    // True iff a real system header named `name` exists in a glibc/libstdc++
    // include dir (i.e. NOT the compiler-owned freestanding dir, and not a
    // madc-own header with no real twin). Used to bypass embedded system-library
    // shims to the real headers while keeping madc-own + freestanding embedded.
    bool embedded_header_is_system_library_shim(const std::string &name) const;
    bool is_dynamic_symbol_allowed(const std::string &name) const;
    bool is_known_namespace(const std::string &name) const;
    Variable *runtime_eval_scope_target(Variable *var) const;
    Variable *runtime_eval_scope_public_target(Variable *var);
    void collect_runtime_eval_scope_variables(std::vector<Variable *> &out) const;
    // The variable whose declaration INITIALIZER is currently being parsed
    // (saved/restored around the init-expression parse, so nesting is safe).
    // Runtime-eval scope capture must skip it: `int x = madc::eval_*(…)`
    // would otherwise capture the uninitialized x AND emit the capture
    // setter ahead of x's C declaration.
    Variable *decl_init_self = NULL;
    void set_namespace_preference(const std::vector<std::string> &order, TokenBase *tb = NULL);
    Variable *find_namespace_member(const std::string &ns_name, const std::string &member_name);
    Variable *resolve_preferred_identifier(class TokenIdent *ident_tb, bool expression_head);
    void set_expression_context_root(const madc::value *root);
    void clear_expression_context_root();
    bool has_expression_context_root() const;
    TokenBase *resolve_expression_context_identifier(class TokenIdent *ident_tb);
    TokenBase *resolve_expression_context_member(TokenBase *lhs, class TokenIdent *member_tb);
    std::string current_source_directory();
    bool include_already_seen(const std::string &path);
    std::string resolve_include_path(const std::string &incfile, bool is_system);
    std::string resolve_include_next_path(const std::string &incfile);
    bool is_system_header_path(const char *path) const;
    std::string expandIfMacros(const std::string &raw);
    bool should_tokenize_include(const std::string &path);
    bool auto_include_standard_identifier(const std::string &word);
    void inject_pending_auto_includes();
    void expand_pending_auto_include_macros(size_t original_start);
    std::vector<TokenBase *> tokenize_auto_include_define(const std::string &value,
							  const TokenBase *origin);
    void mark_embedded_include_flag(const std::string &incfile);
    void push_runtime_scope();
    void pop_runtime_scope();
    static Program *active_runtime_program();
    bool runtime_eval_source(const std::string &source_text,
			     madc::value &result,
			     const std::string &display_name = "__madc_runtime_eval",
			     const madc::value *context = NULL,
			     const char *wrapper_return_type = NULL);
    bool runtime_eval_expression(const std::string &expression,
				 madc::value &result,
				 const std::string &display_name = "__madc_runtime_eval_expression",
				 const madc::value *context = NULL);

    Variable *addFunction(std::string, datatype_vec_t, fVOIDFUNC, bool isMethod=false);

    // manage compound nesting
    void pushCompound();
    void popCompound();

    // generate tokens
    TokenBase *getToken();
    TokenBase *getRealToken();
    void consume_directive_line_tail();
//  TokenProgram *tokenize(std::istream &);
    TokenProgram *tokenize(const char *);
    // Full-fidelity source reconstruction from the token stream (requires
    // keep_trivia set before tokenizing). See TokenBase::leading_trivia.
    std::string reconstruct_source();
    TokenProgram *tokenize_buffer(const std::string &source_text,
				  const std::string &display_name);
    // C/C++ translation phase 6: adjacent string literals concatenate.
    // Funnel every tokens.push_back through this helper so an
    // included `SYSTEM_DIR "file.dat"` (= `"../system/" "file.dat"`)
    // ends up as one merged literal, not two adjacent tokens whose
    // first one gets dropped by parser exStack semantics.
    void push_token_with_literal_concat(TokenBase *tb);

    // for debugging
    void printt(TokenBase *);
//  void showerror(std::istream &);

    // accessing token queue
    inline TokenBase *peekToken() { if (tokens.empty()) return NULL; return tokens.front(); }
    inline TokenBase *prevToken() { return _prv_token; }
    inline TokenBase *curToken()  { return _cur_token; }
    inline void resetPrevToken() { _prv_token = NULL; }
    inline void pushToken(TokenBase *t) { tokens.push_front(t); }

    inline bool keywordStartsUnaryOperandContext(TokenID id)
    {
	return id == TokenID::tkRETURN
	    || id == TokenID::tkCASE
	    || id == TokenID::tkTHROW;
    }

    // helper: is prevToken in a position where the next operator would be unary?
    // true when prevToken is NULL, ;, {, (, ,, =, or any operator except ) ],
    // and postfix ++/--
    inline bool isUnaryPosition()
    {
	if ( !_prv_token ) return true;
	TokenID id = _prv_token->id();
	if ( id == TokenID::tkSemi || id == TokenID::tkOpBrc
	||   id == TokenID::tkOpBrk || id == TokenID::tkComma
	||   id == TokenID::tkAssign ) return true;
	if ( _prv_token->is_operator()
	&&   id != TokenID::tkClBrk && id != TokenID::tkClSqr
	&&   id != TokenID::tkInc && id != TokenID::tkDec ) return true;
	// Only expression-leading keywords open a unary operand context.
	// Contextual keyword identifiers such as `class` can also appear as
	// member names; after `p->class`, a following `&&` is binary logic,
	// not GNU label-address syntax.
	if ( _prv_token->type() == TokenType::ttKeyword )
	    return keywordStartsUnaryOperandContext(id);
	return false;
    }
    // helper: is prevToken in a position where the next operator would be postfix?
    // true when prevToken is ), ], or a non-operator value token
    inline bool isPostfixPosition()
    {
	if ( !_prv_token ) return false;
	TokenID id = _prv_token->id();
	if ( _prv_token->type() == TokenType::ttKeyword )
	    return !keywordStartsUnaryOperandContext(id);
	// Symbols that open or continue expression contexts aren't values
	// either — `{`, `(`, `,`, `;`, `=` mean the next `-` / `!` is
	// unary, not binary. Without this guard `int x[] = { -5 };`
	// converted TokenNeg → TokenSub at the unary-after-`{` slot and
	// emitted a binary subtraction missing its left operand.
	if ( id == TokenID::tkOpBrc || id == TokenID::tkOpBrk
	||   id == TokenID::tkComma || id == TokenID::tkSemi
	||   id == TokenID::tkAssign )
	    return false;
	return id == TokenID::tkClBrk || id == TokenID::tkClSqr
	    || id == TokenID::tkInc || id == TokenID::tkDec
	    || !_prv_token->is_operator();
    }
    inline TokenBase *nextToken()
    {
	if ( tokens.empty() )
	    throw "Unexpected end of data";
        _prv_token = _cur_token;
	_cur_token = tokens.front();
//	DBG(cout << "nextToken(" << (int)ret->type() << ", " << (int)ret->id() << ')' << endl);
	tokens.pop_front();
	// Update global parse position so newly created tokens inherit it
	if ( _cur_token ) {
	    TokenBase::_parse_file   = _cur_token->file;
	    TokenBase::_parse_line   = _cur_token->line;
	    TokenBase::_parse_column = _cur_token->column;
	}
	return _cur_token;
    }
    // parse tokens into AST
    bool load_file(const char *fname);
    bool load_buffer(const std::string &source_text,
		     const std::string &display_name);
    bool parse(TokenProgram *);
    TokenBase *parse_expression_unit(TokenProgram *);
    void parseIdentifier(TokenIdent *);
    void parseFunction(DataDef &, std::string &, DataDefCLASS *owner_class = NULL,
		       std::vector<DataDef *> *multi_ret = NULL,
		       bool return_ref = false,
		       std::string return_typedef_alias = std::string(),
		       bool static_class_method = false);
    TokenBase *parseKeyword(TokenKeyword *);
    TokenBase *parseCallFunc(TokenCallFunc *);
    TokenBase *parseCallMethod(TokenCallMethod *);
    // Consume the operator symbol token(s) following an `operator` keyword token
    // and return the canonical operator-function-id name ("operator<",
    // "operator()", "operatornew", "operator[]", …). The `operator` token itself
    // is the argument; its trailing symbol(s) are consumed from the stream.
    // When `consumed` is non-null, each consumed symbol token is appended to it
    // (so angle-bracket scanners can re-emit the operator-id verbatim).
    std::string parseOperatorId(TokenBase *operator_tok,
				std::vector<TokenBase *> *consumed = NULL);
    // True when `t` is the `operator` keyword that introduces an
    // operator-function-id. The scanners use this so an operator symbol
    // (`operator<`, `operator>>`, …) is treated as part of a NAME, never as an
    // angle-bracket / delimiter — fixing the operator< template mis-parse in
    // ONE place instead of every hand-rolled scanner.
    bool isOperatorIdStart(TokenBase *t);
    // Stream form of the balanced-delimiter step: `t` was already pulled via
    // nextToken(). Update `d`; if `t` is an operator-id keyword, consume its
    // trailing symbol token(s) from the stream (appended to `extra` if given so
    // a token-collecting caller can re-emit the full operator-function-id).
    void delimStepStream(TokenBase *t, DelimDepth &d,
			 std::vector<TokenBase *> *extra = NULL);
    // Constant-expression evaluator (recursive descent over the token stream).
    // Each rung consumes from the stream via nextToken()/peekToken() and folds
    // an integer-constant-expression — used for array dimensions, bit-field
    // widths, case labels, static_assert, enum values, etc. The rungs follow
    // C operator precedence; `parse_constant_integer_expression` is the entry.
    int64_t parse_constant_primary();
    int64_t parse_constant_mul();
    int64_t parse_constant_add();
    int64_t parse_constant_shift();
    int64_t parse_constant_rel();
    int64_t parse_constant_eq();
    int64_t parse_constant_band();
    int64_t parse_constant_bxor();
    int64_t parse_constant_bor();
    int64_t parse_constant_land();
    int64_t parse_constant_lor();
    // Short-circuit token-skip: consume (without evaluating) the RHS operand of
    // a `&&`/`||` whose result the LHS already determines. C++ [expr.const]: the
    // skipped operand need not be a constant expression. stop_at_and=true for a
    // `&&` RHS (a bor-operand, ends at the next `&&`); false for a `||` RHS (a
    // land-operand, spans `&&`). Keeps the cursor positioned for the caller.
    void skip_const_logical_operand(bool stop_at_and);
    int64_t parse_constant_ternary();
    int64_t parse_constant_integer_expression();
    // `if constexpr` support (C++17 [stmt.if]/2). The stream is positioned just
    // AFTER the condition's `(`; collect the balanced condition tokens (consuming
    // the matching `)`) and fold them to a constant. Returns true + value on a
    // successful fold; on failure restores the stream (condition tokens + `)` back)
    // and returns false so the caller falls back to a runtime `if`.
    bool fold_if_constexpr_condition(int64_t &out);
    // Skip ONE statement's tokens (a `{...}` block, balanced, or a single statement
    // through its terminating `;`) WITHOUT parsing — so a discarded `if constexpr`
    // branch is never instantiated or emitted.
    void skip_discarded_statement();
    // Speculatively fold a static-member initializer ('=' already consumed) to a
    // constant int, expecting ';'. Consumes the initializer on success, restores
    // and returns false otherwise. See parser.cpp.
    bool capture_constant_initializer_value(int64_t &out);
    // Parse a bit-field width `: N` (the ':' already consumed) for a member of
    // integer type `member_dd`; `named` rejects a zero width; `target` supplies
    // the storage-size rule. Shared by the struct and class body parsers.
    size_t parse_bitfield_width(TokenBase *loc, DataDef *member_dd, bool named, DataDefSTRUCT &target);
    // Capture a C++11 default member initializer (NSDMI: `= expr` / `{...}`) that
    // begins at `tn` (the token after a data-member declarator), parse it in an
    // isolated stream, and record it under member name `mname` on `dds`. Returns
    // the token following the initializer (the `,`/`;`), or `tn` unchanged if `tn`
    // does not begin an initializer. Shared by the struct and class body parsers.
    TokenBase *capture_member_default_init(TokenBase *tn, DataDefSTRUCT *dds,
					   const std::string &mname);
    // Array-dimension classification: decide whether the upcoming `[ … ]`
    // dimension is a VLA (needs a runtime value) or a constant fold.
    // `bracket_dim_needs_runtime_value` is the entry; the other three are its
    // helpers (speculative constant-parse, runtime-name scan, sizeof/alignof
    // fold-query probe). Scans the token stream from the current position.
    bool bracket_dim_constant_expression_parses();
    bool bracket_dim_uses_runtime_value(
		const std::set<std::string> *runtime_names = NULL);
    bool bracket_dim_has_constant_fold_query();
    bool bracket_dim_needs_runtime_value(
		const std::set<std::string> *runtime_names = NULL);
    // After a method call's arguments are parsed, re-bind `tc` to the overload
    // of `cls`'s method `id` that best matches the argument types (the initial
    // findMethod() bound the first by-name match). Returns `tc` unchanged when
    // it already names the best overload, or a fresh TokenCallMethod on `recv`
    // bound to the winning overload (same parameters / parent_expr / position).
    class TokenCallMethod *reselect_method_overload(class TokenCallMethod *tc,
		Variable &recv, class DataDefCLASS *cls, const std::string &id);
    // Static-member-call analogue of reselect_method_overload: a qualified
    // static call (`Owner::m(args)`) resolves its callee by name+arity BEFORE
    // the args are parsed, so once the arg types are known reselect the overload
    // by [over.match] + [temp.func.order] (findMethodOverload's partial-order
    // tiebreak). Returns a NEW TokenCallFunc bound to the better overload (with
    // the member-template instantiation hooks re-run on it) when one is found,
    // else the original `tc`.
    class TokenCallFunc *reselect_static_member_overload(class TokenCallFunc *tc,
		class DataDefCLASS *owner, const std::string &member);
    // The class-object DataDef an operand expression DENOTES for operator /
    // overload resolution: its datadef's class, or — for a reference-typed
    // expression / a REFERENCE variable (`const A &p`, vfREFERENCE stored as
    // DataDefPTR(A)) — the referenced class. A plain `A*` pointer operand
    // stays NULL: only the reference representation is transparent here.
    class DataDefCLASS *operand_object_class(TokenBase *operand);
    // The VALUE view of an operand's type (reference expression / vfREFERENCE
    // variable -> the referenced type) for deduction and overload ranking.
    // A CALL operand types by its RESOLVED callee's return — see
    // resolved_call_funcdef.
    DataDef *operand_value_datadef(TokenBase *operand);
    // The RESOLVED callee of a call token whose parse-bound Variable may be an
    // arbitrary member of a late-bound namespace overload set (overloads /
    // fn-template instantiations register after the call site parses). Re-ranks
    // the set with the call's argument value types; the parse-side mirror of
    // CirBuilder::call_target_funcdef. Returns the bound FuncDef when no set
    // applies; sets *no_winner when a set applies but no candidate is viable.
    FuncDef *resolved_call_funcdef(class TokenCallFunc *tc,
				   bool *no_winner = NULL);
    // Type an operator expression on a class-object operand with the operator's
    // return type (Part A of generic operator-overload support). No-op unless the
    // left operand is a class object declaring the matching binary operator, or
    // (W2) a captured FREE namespace operator on the operand classes returns a
    // class by value (std::operator+ on strings).
    void resolve_object_operator_type(class TokenOperator *to);
    // Array-to-pointer decay ([conv.array]) for an operator operand: returns the
    // decayed `element *` type for a fixed-array variable / array member /
    // array-typed expression, else NULL. See parser.cpp.
    DataDef *array_decay_pointer(TokenBase *operand);
    // The return CLASS of a captured FREE namespace binary operator on class
    // operands whose return is a class BY VALUE deducing to one of the operand
    // classes. Structural template-head check only — full deduction, overload
    // arbitration, and symbol binding happen at lowering
    // (CirBuilder::resolve_free_operator_byvalue). NULL = no such operator.
    DataDef *free_binary_operator_return_class(class DataDefCLASS *lc,
		const std::string &opname, TokenBase *right);
    // Literal-lhs sibling (`"pre" + s`): param[0] non-class pointer/value,
    // param[1] the class by const-ref; returns the by-value return class.
    DataDef *free_binary_operator_return_class_nonclass_lhs(TokenBase *left,
		const std::string &opname, TokenBase *right);
    TokenBase *parseCompound();
    TokenBase *parseStatement(TokenBase *);
    TokenBase *parseDeclaration(TokenDataType *, bool is_static = false);
    DataDefPTR *getPointerType(DataDef *base);
    DataDefREF *getReferenceType(DataDef *base);
    // The return type to CONSTRUCT a FuncDef with: a DataDefREF for a
    // reference return (routed through getReferenceType — the single
    // reference-creation/collapse path), else the bare type. Centralizes the
    // "a reference return is born as a DataDefREF" decision so FuncDef::returns
    // holds the real reference type, not a bare referent + parallel flag
    // (first-class refs Phase 2).
    DataDef &returnDecl(DataDef &dd, bool is_ref)
	{ return is_ref ? *(DataDef *)getReferenceType(&dd) : dd; }
    // Fold a trailing template-argument declarator suffix (`*`, `&`, `&&`) off the
    // token stream into the argument's type, returning the wrapped type token.
    // One owner of the rule, shared by every template-argument parser — a pointer
    // OR reference type is a valid template argument (`Vec<T*>`,
    // `conditional<b, int&, long>`, `__conditional_t<…, remove_reference_t<R>&&, …>`).
    TokenDataType *fold_template_arg_declarator(TokenDataType *adt, TokenBase *origin);
    // Resolve a fn-template parameter's DEFAULT token run to a concrete type,
    // substituting the already-bound type parameters in, then resolving in an
    // isolated token stream (handles trait-expression / template-id defaults like
    // `R = __conditional_t<...>`). Returns NULL for a still-dependent default.
    DataDef *resolve_template_param_default_type(
		const std::vector<TokenBase *> &default_tokens,
		const std::map<std::string, DataDef *> &binding,
		DataDefCLASS *owner);
    // Consume a declarator's pointer-star run: a sequence of `*` interleaved with
    // cv-qualifiers (const/volatile/restrict). Each `*` on a NON-fn-ptr base wraps
    // `dd` via getPointerType (the type reflects the indirection); a DataDefFPTR
    // base (function-type typedef) is left UNWRAPPED — fn-ptr call detection keys
    // on the type being DataDefFPTR, so the count is returned for the caller to
    // record (Variable::fnptr_explicit_stars) and the emitter renders exactly the
    // stars written. Returns the number of `*` consumed; if out_const_after_star
    // is non-NULL, sets it to whether a `const` followed the last `*` (top-level
    // const pointer `T * const p`). One shared loop instead of the copy-pasted
    // `while(tkMul){...}` declarator loops.
    int consume_declarator_stars(DataDef *&dd, bool *out_const_after_star = nullptr);
    // parse a `(params)` list after the opening '(' has been consumed; used by
    // function-pointer typedefs. Builds a FuncDef with the given return type.
    // Parameter names are accepted but discarded. Stops after consuming ')'.
    FuncDef *parseFnPtrParams(DataDef &returns);
    // The FuncDef giving a CALL's signature: the variable's own FuncDef, or
    // the target signature behind a function-pointer type (DataDefFPTR).
    // NULL when the variable isn't callable-typed. Parse-time twin of the CIR
    // builder's call_target_funcdef — never blind-cast var.type to FuncDef
    // (a fn-ptr call's DataDefFPTR read as FuncDef is UB: SMAUG's
    // `(*skill->spell_fun)(...)` arity check read garbage).
    static FuncDef *call_signature_funcdef(const Variable &var);
    // Function-pointer MEMBER declarator tail: parses `name ) ( params )`
    // after the leading `RET ( *` was consumed; returns the DataDefFPTR and
    // sets mname. Shared by the top-level and nested-aggregate struct member
    // parsers (e.g. glibc sigevent's `void (*_function)(__sigval_t);` inside
    // an anonymous union).
    DataDefFPTR *parse_fnptr_member_tail(DataDef &returns, std::string &mname,
					 TokenBase *open_tok);
    TokenBase *parseExpression(TokenBase *, bool conditional=false,
			       bool ternary_branch=false,
			       bool stop_on_closing_paren=false,
			       int initial_brackets=0,
			       bool push_back_comma=false,
			       bool cast_operand=false);
    // Control-flow signal an extracted parseExpression switch-arm handler
    // returns to the shunting-yard loop, one-to-one with the arm's original
    // inline control flow: Break = fall to the per-token epilogue (peek/advance),
    // Continue = re-enter the loop (the arm already advanced `tb`), Redo =
    // re-dispatch the (rewritten) `tb` without advancing (was `goto
    // redo_expression_token`), Done = terminate the expression (was
    // `done = true`). Lets the giant per-type arms move into named methods
    // while the loop keeps owning the stacks and the goto/continue.
    enum class ExprStep { Break, Continue, Redo, Done, Return };
    // ttDataType switch-arm of parseExpression: a type name appearing in
    // expression context (member name after ./->, contextual identifier /
    // variable, functional-cast, ns-qualified callee, or an inline declaration).
    // `tb` is in/out so a Redo can hand back a rewritten token.
    ExprStep parseExpr_dataTypeArm(TokenBase *&tb,
				   std::stack<TokenBase *> &exStack,
				   std::stack<TokenBase *> &opStack,
				   TokenCpnd *code);
    // ttSymbol switch-arm of parseExpression: statement/expression terminators
    // and the C comma operator (`;` ends; `,` either builds a comma-expression
    // node inside parens or ends the expression). Read-only `brackets`.
    ExprStep parseExpr_symbolArm(TokenBase *tb,
				 std::stack<TokenBase *> &exStack,
				 std::stack<TokenBase *> &opStack,
				 int brackets, bool push_back_comma);
    // ttIdentifier switch-arm of parseExpression: the largest arm — identifier
    // resolution in expression context (variables, function/method calls,
    // member access, template-ids, qualified/namespaced names, casts,
    // new/sizeof-family, …). `tb` is in/out as it advances through the stream.
    // ttKeyword falls through into this for keywords that are contextual
    // identifiers.
    ExprStep parseExpr_identifierArm(TokenBase *&tb,
				     std::stack<TokenBase *> &exStack,
				     std::stack<TokenBase *> &opStack,
				     TokenCpnd *code);
    // ttMultiOp/ttOperator switch-arm of parseExpression: the operator
    // shunting-yard core (precedence climbing, unary/binary disambiguation,
    // parentheses/subscript/ternary/cast/comma). Mutates `brackets` (by ref)
    // as it tracks paren depth.
    // `result` carries the value for an ExprStep::Return exit (an early
    // return from parseExpression that bypasses the operator-stack drain).
    ExprStep parseExpr_operatorArm(TokenBase *&tb,
				   std::stack<TokenBase *> &exStack,
				   std::stack<TokenBase *> &opStack,
				   int &brackets, TokenCpnd *code,
				   bool conditional, bool ternary_branch,
				   bool stop_on_closing_paren,
				   int initial_brackets, bool push_back_comma,
				   TokenBase *&result, bool cast_operand=false);
    // Old-style (K&R) parameter declarations: detection + parsing of the
    // `int f(a, b) int a; char *b; { … }` form (C only). The detectors peek
    // the stream; parse_old_style_parameter_declaration fills param_types.
    bool old_style_parameter_head_has_declaration_suffix();
    bool is_old_style_parameter_head(TokenBase *tb);
    bool is_old_style_parameter_declaration_start(TokenBase *tb);
    DataDef *parse_old_style_parameter_base(TokenBase *&nt);
    void parse_old_style_parameter_declaration(TokenBase *nt,
		const std::vector<std::string> &param_ids,
		std::map<std::string, DataDef *> &param_types);
    bool scan_old_style_definition_suffix(std::vector<TokenBase *> &suffix);
    // Namespace resolution helpers: walk the enclosing-namespace chain to find
    // a member, resolve a bare name against the active namespace scope, and
    // report the namespace the current C++ scope looks up in.
    Variable *find_namespace_member_in_scope_chain(const std::string &ns_name,
						   const std::string &member_name);
    std::string resolve_namespace_name_in_scope(const std::string &name);
    std::string active_cpp_lookup_namespace();
    // static_assert: parse the statement form (folds the constant condition,
    // throws with the message on failure), consume the deferred form inside an
    // uninstantiated template body, and consume the class-scope declaration.
    void consume_deferred_static_assert_statement(TokenBase *tb);
    TokenBase *parse_static_assert_statement(TokenBase *tb);
    void consume_class_static_assert_declaration(TokenBase *tb);
    // Type queries: sizeof/alignof/typeof and the runtime type-query operators.
    // resolve_type_query_datadef resolves the operand to a DataDef (+ optional
    // folded value); evaluate_type_query folds sizeof/alignof; parse_typeof_datatype
    // yields the typeof()'d type; try_parse_dynamic_type_query handles the runtime
    // form; try_parse_constant_offsetof_address folds offsetof-style addresses.
    DataDef *resolve_type_query_datadef(TokenBase *type_tb,
					const std::string &op_name,
					bool &have_value, size_t &query_value);
    size_t evaluate_type_query(TokenBase *op_tb, const std::string &op_name);
    // Type-trait builtins (__is_class/__is_base_of/…): parse `( type-list )` and
    // fold to a bool constant token. See parser.cpp for the supported (faithful)
    // set; unsupported traits are not recognized (clear error, never a wrong bool).
    TokenBase *evaluate_type_trait(TokenBase *op_tb, const std::string &name);
    TokenBase *try_parse_dynamic_type_query(TokenBase *op_tb,
					    const std::string &op_name);
    TokenDataType *parse_typeof_datatype(TokenBase *op_tb);
    bool try_parse_constant_offsetof_address(int64_t &out);
    // Named C++ casts (static_cast/reinterpret_cast/const_cast/dynamic_cast):
    // parse the expression form and the constant-folded form; plus the
    // C-style cast operand helpers (deref/function-call/literal materialization).
    int64_t parse_constant_named_cpp_cast(TokenBase *cast_tb,
					  const std::string &cast_name);
    bool try_parse_constant_functional_cast(TokenBase *type_tb,
					    int64_t &out);
    TokenBase *parse_named_cpp_cast(TokenBase *cast_tb,
				    const std::string &cast_name);
    TokenBase *parse_cast_unary_deref_operand(TokenBase *star);
    TokenBase *parse_cast_function_call_operand(TokenBase *head);
    TokenBase *materialize_cast_literal_operand(TokenBase *tb);
    // Template-machinery leaf consumers: recognize a template-argument-list
    // close (`>` or split `>>`), detect whether we're in an instantiated member
    // body, consume a template-parameter type suffix, and collect a template
    // default-argument token run.
    bool parsing_template_instantiated_member_body();
    bool consume_template_close(TokenBase *tok);
    bool consume_template_parameter_type_suffix();
    std::vector<TokenBase *> collect_template_default_argument();
    // Template-machinery core: <…> argument scanning, template-id / alias /
    // opaque instantiation, dependent/opaque member-type materialization,
    // deferred-instantiation completion, and the template-decl skippers.
    void skip_template_id_suffix();
    // Consume a C++20 requires-clause at the current token (constraints are
    // not evaluated — madc has no concepts; the constrained declaration
    // parses as if unconstrained). Returns true if a clause was consumed.
    bool skip_requires_clause();
    // The constraint scanner behind it (the `requires` keyword already
    // consumed) — also used for trailing requires-clauses by the
    // template-declaration skipper.
    void skip_constraint_expression();
    void skip_template_nonclass_declaration(TokenBase *first,
					    std::vector<TokenBase *> *seen = NULL);
    void capture_extern_template_class_instantiation();
    void apply_template_call_return_inference(TokenCallFunc *tc);
    DataDef *resolve_namespace_fn_template_call_return_type(TokenCallFunc *tc,
							    bool *ret_ref);
    // Key-based core of the above: resolve a free/namespace function-template
    // call's return type from "ns::name" + the explicit type arguments. Called
    // by the TokenCallFunc entry AND recursively for a `decltype(inner_call)`
    // return (declval's `decltype(__declval<_Tp>(0))`). `depth` bounds the
    // recursion. No emission — resolves purely from the retained decl tokens.
    DataDef *resolve_fn_template_return_by_key(const std::string &key,
				const std::vector<DataDef *> &explicit_args,
				bool *ret_ref, int depth);
    // Resolve `decltype ( IDENT < targs > ( args ) )` (substituted tokens) by
    // recursing into IDENT's template return type in namespace `ns`. No emit.
    DataDef *resolve_decltype_call_return(const std::vector<TokenBase *> &sub,
				const std::string &ns, bool *ret_ref, int depth);
    TokenBase *collect_template_argument_spelling(TokenBase *first,
						  std::string &spelling,
						  std::vector<TokenBase *> *tokens_out = NULL);
    std::vector<TokenBase *> collect_template_class_prefix();
    TokenBase *consume_template_type_arg_qualifiers(TokenBase *tb,
						    std::string &spelling);
    TokenDataType *instantiate_template_use(const std::string &tname,
					    TokenBase *tb,
					    const std::string &ns_hint = std::string(),
					    DataDefCLASS *owner_hint = NULL);
    TokenDataType *instantiate_template_alias_use(const std::string &tname,
						  TokenBase *tb,
						  const std::string &ns_hint = std::string(),
						  DataDefCLASS *owner_hint = NULL);
    // True iff EVERY collected alias-use argument is non-dependent (concrete):
    // each non-type arg folds to a constant and each type arg resolves to a
    // type with no unresolved dependent surface. Lets instantiate_template_alias_use
    // tell a genuine SFINAE failure (concrete args, absent `::type`) from a
    // deferred dependent use.
    bool alias_use_args_all_concrete(const TemplateAliasDef &td,
			 const std::vector<std::vector<TokenBase *> > &arg_tokens);
    // Resolve a template-id `Name<...>` to its concrete type: an alias template
    // first, then a class template (the order every call site used by hand).
    // Single seam for the namespace hint so qualified uses pick the right
    // same-named variant. Returns NULL if Name is not a (alias-or-class)
    // template-id.
    TokenDataType *instantiate_template_id(const std::string &tname,
					   TokenBase *tb,
					   const std::string &ns_hint = std::string(),
					   DataDefCLASS *owner_hint = NULL);
    TokenDataType *instantiate_opaque_template_use(TemplateDef &td,
						   const std::string &tname,
						   TokenBase *tb);
    TemplateAliasDef *find_template_alias(const std::string &name,
					  const std::string &ns_hint = std::string(),
					  DataDefCLASS *owner_hint = NULL);
    void register_template_alias(const TemplateAliasDef &td);
    DataDefCLASS *materialize_dependent_member_type(DataDefCLASS *owner,
						    const std::string &member_name);
    DataDefCLASS *materialize_opaque_class_type(const std::string &name,
						const std::string &canonical);
    DataDef *dependent_deref_result_type(DataDef *dd);
    void complete_pending_template_instantiations(const std::string &class_name);
    bool request_template_instantiation_completion(const std::string &mangled_name);
    bool template_declared_in_namespace(const std::string &name,
					const std::string &ns_name);
    TokenBase *consume_unresolved_dependent_call(TokenBase *open);
    // Type-token resolution: resolve a declared / namespaced / typename-qualified
    // type name (optionally consuming the tokens, allowing lazy types, walking a
    // class-member chain) to its TokenDataType.
    TokenDataType *resolve_typename_type_token(TokenBase *first,
					       bool allow_lazy_types,
					       TokenBase *typename_tb);
    TokenDataType *resolve_class_member_type_chain(DataDefCLASS *owner,
						   TokenBase *owner_tb);
    TokenDataType *resolve_member_chain_or_type(TokenDataType *type_tok,
						TokenBase *tb,
						bool consume_class_member_chain);
    TokenDataType *resolve_declared_type_token(TokenBase *tb,
					       bool consume_ns_tokens,
					       bool allow_lazy_types,
					       bool consume_class_member_chain = true);
    // Resolve a TYPE that spans a token RANGE (e.g. a member-template return
    // type `std::pair<iterator, bool>`) through the canonical type resolver,
    // in an isolated token stream so the live parse position is untouched.
    // Returns the resolved DataDef, or NULL when the range is not a type.
    DataDef *resolve_type_token_range(const std::vector<TokenBase *> &toks,
				      size_t start, size_t end);
    TokenDataType *resolve_namespaced_type_token(TokenBase *tb, bool consume_tokens);
    // Type-name resolution helpers: look up a named DataDef (struct/typedef/lazy),
    // a current-class type alias, a variable matching a contextual type name, and
    // the class scope an expression name resolves to.
    DataDef *resolve_named_datadef(const std::string &name);
    DataDef *resolve_current_class_type_alias(const std::string &name);
    bool resolve_current_class_static_member_const_value(const std::string &name, int64_t &out);
    bool fold_constant_qualified_member(TokenBase *first, int64_t &out);
    Variable *find_variable_for_contextual_type_name(const std::string &name);
    DataDefCLASS *resolve_expression_class_scope(const std::string &name);
    // Class-body parsing: detect when a struct body needs the class parser /
    // an inline enum follows, parse anonymous aggregates, bind a declared C++
    // member symbol, mint a unique overload symbol, collect a deferred function
    // body's tokens and parse them later, and promote a struct base to a class.
    bool cpp_struct_body_needs_class_parser(const std::string &tag_name,
					    TokenBase *after_tag);
    bool consume_anonymous_aggregate_open(bool &packed);
    DataDefSTRUCT *parse_class_anonymous_aggregate(TokenBase *kw);
    void parse_class_anonymous_aggregate_members(DataDefSTRUCT *agg,
						 TokenBase *loc);
    bool class_body_enum_definition_follows();
    void bind_declared_cpp_symbol(DataDefCLASS *ddc, Variable *mvar,
				  CppSymKind kind, const std::string &mname,
				  bool is_operator);
    std::string unique_overload_symbol(std::string base);
    std::vector<TokenBase *> collect_compound_body_tokens(TokenBase *open);
    void enqueue_deferred_function_body(Variable *var,
					Method *method, TokenBase *open,
					const std::vector<TokenBase *> *trailing_ret = NULL);
    void parse_deferred_function_body(DeferredFunctionBody &body);
    void parse_deferred_function_bodies(std::vector<DeferredFunctionBody> &bodies);
    // Lazy member-function-body instantiation: parse a single deferred body by
    // emit symbol on first ODR-use (returns the materialized TokenFunc, or NULL
    // if the symbol has no deferred body / was already parsed).
    TokenFunc *parse_deferred_lazy_body(const std::string &emit_symbol);
    bool has_deferred_lazy_body(const std::string &emit_symbol) const
	{ return deferred_lazy_bodies.find(emit_symbol) != deferred_lazy_bodies.end(); }
    DataDefCLASS *promote_struct_base_to_class(const std::string &name,
					       DataDef *dd);
    // GNU/C23 attribute consumers: skip/collect __attribute__((…)) (optionally
    // capturing attrs / alias target / alignment / vector size), an asm("label")
    // alias, [[…]] C23 attributes, a vector_size attribute value, and `...`.
    TokenBase *consume_gnu_attributes(TokenBase *nt,
				      std::set<std::string> *attrs = NULL,
				      std::string *alias_target = NULL,
				      size_t *explicit_align = NULL,
				      size_t *vector_bytes = NULL);
    // Set by consume_gnu_attributes on optimize("-fno-strict-aliasing") in any
    // position; consumed (and cleared) by the function-declaration parse.
    bool pending_no_strict_aliasing;
    TokenBase *consume_gnu_asm_label(TokenBase *nt, std::string *alias_target);
    // Skip (or lower the recognized `=r`/`+r`/`+m`/... copy shapes of) a GNU
    // asm STATEMENT. `tb` is the asm introducer (identifier or reserved
    // tkCPPKEYWORD spelling). Shared by both the ttIdentifier and ttKeyword
    // arms of parseStatement so reserving `asm` as a keyword does not lose the
    // statement-level skip.
    TokenBase *skip_gnu_asm_statement(TokenBase *tb);
    void skip_c23_attributes();
    size_t parse_gnu_vector_size_attribute();
    void consume_typedef_gnu_attributes(std::string *mode_name = NULL,
					size_t *vector_bytes = NULL);
    bool consume_ellipsis();
    // Parameter-signature / qualified-declarator parsing: count queued call args,
    // resolve a qualified class owner, parse a qualified declarator part, split an
    // upcoming function parameter list, resolve/parse parameter signatures, find a
    // matching constructor, and parse a qualified special-member definition.
    size_t count_queued_call_arguments();
    DataDefCLASS *resolve_qualified_class_owner(const std::vector<std::string> &scope_parts);
    std::string parse_qualified_declarator_part(TokenBase *part_tb);
    bool split_upcoming_function_params(std::vector<std::vector<TokenBase *> > &params);
    DataDef *resolve_param_type_from_tokens(const std::vector<TokenBase *> &param,
					    size_t &idx);
    bool parse_param_sig_from_tokens(std::vector<TokenBase *> param,
				     ParsedParamSig &sig);
    bool upcoming_param_signatures(std::vector<ParsedParamSig> &sigs);
    Variable *find_constructor_for_upcoming_params(DataDefCLASS *owner);
    bool parse_qualified_special_member_definition(TokenBase *first_tb);
    // Assorted parse helpers (expression/declaration/statement support).
    DataDef *effective_pointer_type_for_member_access(TokenBase *tb);
    DataDef *parse_typedef_array_suffix(DataDef *base_dd,
					const std::string &alias_name,
					TokenBase *err_tok);
    TokenBase *consume_balanced_parenthesized_suffix(TokenBase *open);
    TokenBase *make_expression_context_literal(const madc::value &resolved,
					       TokenBase *src);
    TokenBase *materialize_runtime_struct_size_captures(TokenCpnd *code,
							DataDefSTRUCT *dds, TokenBase *loc);
    TokenBase *materialize_vla_dim_capture(TokenCpnd *code,
					   TokenBase *&dim_expr, TokenBase *loc);
    TokenBase *try_parse_vla_variable_sizeof(TokenBase *op_tb,
					     const std::string &op_name);
    TokenBase *parse_functional_type_expression(TokenBase *type_tb,
						DataDef *type_dd);
    TokenBase *parse_namespace_block(bool inline_namespace);
    TokenBase *parse_parenthesized_expression(const char *context,
					      bool stop_on_closing_paren);
    TokenBase *reference_bind_address_expr(TokenBase *expr,
					   DataDef *referent_type);
    TokenBase *skip_expression_whitespace();
    TokenCASE *parse_switch_label(TokenSWITCH *sw, TokenBase *tn);
    TokenObjTemp *try_parse_functional_ctor(TokenBase *name_tb);
    Variable *resolve_c_identifier(TokenIdent *ident_tb, bool expression_head);
    bool datatype_statement_starts_functional_expr();
    bool datatype_statement_starts_qualified_expr();
    bool is_shared_global_extern_reference(TokenCpnd *code, Variable *var);
    bool next_parenthesized_type_is_compound_literal();
    bool paren_group_is_function_def();
    bool paren_group_is_nonclass_direct_init();
    bool parse_array_designator_initializer(TokenBase *&next_init,
					    size_t &first_index, size_t &last_index);
    bool parse_builtin_types_compatible_operand(TokenBase *type_tb,
						std::string &sig);
    bool resolve_integer_constant(TokenBase *tb, int64_t &out);
    bool token_starts_type_name(TokenBase *tb);
    void configure_nested_function_captures(FuncDef *func);
    void mirror_inline_namespace_into_parent(const std::string &parent_ns,
					     const std::string &inline_ns);
    // Statement-level expression parse: parseExpression + comma-chain.
    // parseExpression treats `,` as a hard stop (callers like for-loop
    // init/incr and call-arg lists rely on this). In statement contexts
    // (`expr1, expr2;` or a brace-less `while (c) e1, e2;` body), the
    // comma is the C comma operator and both expressions must run for
    // side effects. parseExprStmt collects them into a TokenComma chain
    // whose compile() evaluates left for effects and returns right.
    TokenBase *parseExprStmt(TokenBase *);
    // Parse an identifier followed by any chain of postfix operators
    // (->ident / .ident / [expr] / ++ / --) and return the resulting
    // expression node. Stops at the first non-postfix token (binary
    // operator, comma, semicolon, etc.) and pushes it back on the
    // token stream. Used by unary `*` and `&` to avoid
    // parseExpression's greedy consumption of trailing binary ops.
    TokenBase *parsePostfixChain(TokenBase *head);
    TokenBase *parsePostfixChainFrom(TokenBase *result, Variable *var);
    // Parse the operand after unary `&`, preserving C precedence by
    // stopping before trailing binary operators.
    TokenBase *parseAddressOfExpression(TokenBase *ampersand);
    TokenFunc *build_expression_function(TokenProgram *tp,
					 TokenBase *expr,
					 DataDef *return_type,
					 const std::string &function_name,
					 bool have_result,
					 const std::string &result_name = "__madc_expr_value");
    TokenBase *parseLambda();  // parse [](params) { body } lambda expression

    // Compile the parsed program. The asmjit JIT codegen backend has
    // been removed; CIR (madc parse → cir_node → c2mir → MIR) is the
    // sole backend, invoked via madc_cir_execute(). This stub always
    // fails so legacy JIT-pipeline callers degrade cleanly.
    bool compile();

    // AOT object/executable output is unavailable on the CIR backend.
    // These stubs report a clear error and return false; emit C and
    // compile with gcc/clang instead.
    bool save_object(const std::string &path) const;
    bool save_executable(const std::string &path);

    // load a previously saved ELF .o and map it for execution
    struct LoadedObject
    {
	void *code_base;
	size_t code_size;
	std::map<std::string, void *> functions;
	LoadedObject() : code_base(NULL), code_size(0) {}
    };
    LoadedObject loaded_object;
    bool load_object(const std::string &path);
    bool has_loaded_function(const std::string &name) const;
    void *loaded_function_ptr(const std::string &name) const;
    void unload_object();

    // execute the resulting code
    void execute();

    // data management
    DataDef *findType(std::string &);
    Variable *addVariable(TokenCpnd *, DataDef &, std::string &, int c=1, void *init=NULL, bool alloc=true);
    Variable *resolve_global_storage_variable(Variable *var) const;
    Variable *addGlobal(DataDef &d, std::string str, int c=1, void *init=NULL)
    {
	return addVariable(NULL, d, str, c, init, true);
    }
    Variable *findVariable(TokenCpnd *, std::string &);
    Variable *findVariable(std::string &);
    Variable *addLiteral(std::string &);
    Variable *addWideLiteral(std::string &);
//  Method *findMethod(std::string &);
};

class MadcEngine
{
public:
    enum class LogLevel
    {
	emerg,
	alert,
	crit,
	err,
	warn,
	notice,
	info,
	debug
    };

    typedef std::function<void(LogLevel, const std::string &)> LogSink;

    std::istream *input_stream;
    std::ostream *output_stream;
    std::ostream *error_stream;
    std::streambuf *default_input_buf;
    std::streambuf *default_output_buf;
    std::streambuf *default_error_buf;
    std::unique_ptr<std::istringstream> owned_input_buffer;
    std::unique_ptr<std::ostringstream> owned_output_buffer;
    std::unique_ptr<std::ostringstream> owned_error_buffer;
    std::unique_ptr<MadcTeeBuf> output_tee_buf;
    std::unique_ptr<MadcTeeBuf> error_tee_buf;
    bool log_timestamps;
    bool log_level_prefixes;
    LogLevel log_threshold;
    bool log_to_error_stream;
    bool syslog_active;
    std::string syslog_ident;
    int syslog_option;
    int syslog_facility;
    bool file_sink_active;
    std::unique_ptr<std::ofstream> log_file;
    std::string log_file_path;
    size_t log_file_max_bytes;
    int log_file_max_files;
    bool json_sink_active;
    std::unique_ptr<std::ofstream> json_file;
    std::string json_file_path;
    std::vector<LogSink> log_sinks;
    bool verbose = false;
    Program::RegistrationPolicy registration_policy;
    Program::BuiltinRegistry builtin_registry;
    Program::NamespaceRegistry namespace_registry;

    MadcEngine();
    ~MadcEngine();
    std::istream &input();
    std::ostream &output();
    std::ostream &error();
    void bind_input_stream(std::istream &is);
    void bind_output_stream(std::ostream &os);
    void bind_error_stream(std::ostream &os);
    void bind_input_string(const std::string &text);
    void capture_output_to_buffer();
    void capture_error_to_buffer();
    void tee_output_stream(std::ostream &os);
    void tee_error_stream(std::ostream &os);
    void tee_output_to_buffer();
    void tee_error_to_buffer();
    const char *log_level_name(LogLevel level) const;
    std::string format_log_message(LogLevel level, const std::string &message) const;
    bool should_log(LogLevel level) const;
    void write_log(LogLevel level, const std::string &message);
    void write_builtin_sinks(LogLevel level, const std::string &message);
    void add_log_sink(LogSink sink);
    void clear_log_sinks();
    static int syslog_priority_for(LogLevel level);
    void enable_syslog_sink(const char *ident = "madc", int option = -1, int facility = -1);
    void disable_syslog_sink();
    bool enable_file_sink(const std::string &path,
			  size_t max_bytes = 0,
			  int max_files = 5);
    void disable_file_sink();
    void rotate_log_file();
    void reopen_log_file();
    bool enable_json_sink(const std::string &path);
    void disable_json_sink();
    static std::string json_escape(const std::string &s);
    std::string format_json_log_line(LogLevel level, const std::string &message) const;
    void write_syslog_sink(LogLevel level, const std::string &message);
    void write_file_sink(LogLevel level, const std::string &message);
    void write_json_sink(LogLevel level, const std::string &message);

    struct Config
    {
	LogLevel threshold;
	bool timestamps;
	bool level_prefixes;
	bool error_stream;

	bool file_sink;
	std::string file_path;
	size_t file_max_bytes;
	int file_max_files;

	bool syslog_sink;
	std::string syslog_ident;
	int syslog_option;
	int syslog_facility;

	bool json_sink;
	std::string json_path;

	Config()
	    : threshold(LogLevel::debug),
	      timestamps(false),
	      level_prefixes(true),
	      error_stream(true),
	      file_sink(false),
	      file_max_bytes(0),
	      file_max_files(5),
	      syslog_sink(false),
	      syslog_ident("madc"),
	      syslog_option(-1),
	      syslog_facility(-1),
	      json_sink(false)
	{}
    };

    bool apply_log_config(const Config &cfg);
    bool has_output_buffer() const;
    bool has_error_buffer() const;
    std::string output_buffer_str() const;
    std::string error_buffer_str() const;
    void clear_output_buffer();
    void clear_error_buffer();
    void reset_standard_streams();
    void populate_default_registries();
    void configure_program(Program &pgm) const;
    std::unique_ptr<Program> create_program();
    void bind_log_streams();
    static void unbind_log_streams();
};

class MadcLogStreambuf : public std::streambuf
{
public:
    explicit MadcLogStreambuf(MadcEngine::LogLevel lvl);

    void set_engine(MadcEngine *eng) { _engine = eng; }
    MadcEngine *engine() const { return _engine; }
    MadcEngine::LogLevel level() const { return _level; }

protected:
    virtual int overflow(int ch = EOF) override;
    virtual std::streamsize xsputn(const char *s, std::streamsize n) override;
    virtual int sync() override;

private:
    MadcEngine::LogLevel _level;
    MadcEngine *_engine;
    std::string _line;
    void flush_line();
};

class MadcLogStream : public std::ostream
{
public:
    explicit MadcLogStream(MadcEngine::LogLevel lvl);

    void set_engine(MadcEngine *eng) { _buf.set_engine(eng); }
    MadcEngine *engine() const { return _buf.engine(); }
    MadcEngine::LogLevel level() const { return _buf.level(); }

private:
    MadcLogStreambuf _buf;
};

namespace madc
{
    extern MadcLogStream emerg;
    extern MadcLogStream alert;
    extern MadcLogStream crit;
    extern MadcLogStream err;
    extern MadcLogStream warn;
    extern MadcLogStream notice;
    extern MadcLogStream info;
    extern MadcLogStream debug;
}

#define ANSI_RED "\e[1;31m"
#define ANSI_WHITE "\e[1;37m"
#define ANSI_RESET "\e[m"

// Macros for generating extern "C" thin wrappers.
// Usage: MADC_EXTERN_C1(int64_t, php_trim, void *)
// Expands to: extern "C" int64_t __php_trim(void *a) { return php_trim(a); }
#define MADC_EXTERN_C0(ret, name) \
    extern "C" ret __##name(void) { return name(); }
#define MADC_EXTERN_C1(ret, name, T1) \
    extern "C" ret __##name(T1 a) { return name(a); }
#define MADC_EXTERN_C2(ret, name, T1, T2) \
    extern "C" ret __##name(T1 a, T2 b) { return name(a, b); }
#define MADC_EXTERN_C3(ret, name, T1, T2, T3) \
    extern "C" ret __##name(T1 a, T2 b, T3 c) { return name(a, b, c); }
#define MADC_EXTERN_C4(ret, name, T1, T2, T3, T4) \
    extern "C" ret __##name(T1 a, T2 b, T3 c, T4 d) { return name(a, b, c, d); }

#endif // __MADC_H
