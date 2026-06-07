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
    DataDef &returns;
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
    // A GNU nested function is hoisted to a unique top-level C symbol
    // (`enclosing__name__N`); its in-scope local alias keeps the source name
    // (`name`). This is the hoisted symbol every call site must reference.
    // Empty for a non-nested function (call sites use the plain name).
    std::string nested_emit_name;
    // multiple return values (empty = single return via `returns`)
    std::vector<DataDef *> return_types;
    // reference parameter tracking: ref_params[i] == true when parameter i is T&
    std::vector<bool> ref_params;
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
    // reference return: true when the function returns T& (e.g. T& operator[]).
    // `returns` stays the base type T; the value is returned BY ADDRESS (a T*),
    // so the call site is an lvalue (assign stores through it; read derefs it),
    // matching g++. See cir_builder ref-return lowering.
    bool returns_ref;
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
    // For a madc-emitted class operator that shares its name with another
    // overload of DIFFERENT arity (e.g. unary `operator-()` AND binary
    // `operator-(const C&)`, or prefix `operator++()` AND postfix
    // `operator++(int)`), the default `ClassName__operatorX` scheme collides.
    // When set, this is the actual C symbol the method's BODY is emitted as and
    // its CALL sites reference — an arity-disambiguated name. Empty for the
    // common single-overload case (the default scheme is used). Distinct from
    // emit_symbol: this still takes the normal madc-emitted-body path, NOT the
    // extern-binding path that emit_symbol triggers.
    std::string class_emit_name;
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
    // For an externally-bound ctor (emit_symbol set) whose real ABI takes a
    // trailing reference argument that madc has no value for, pass the object's
    // own address (&this) as that trailing arg.
    bool ctor_trailing_self;
    // Initializer order matches member declaration order (avoids -Wreorder).
    FuncDef(DataDef &d) : returns(d), explicit_alignment(0), has_captures(false), returns_ref(false), template_return_param_name(), template_return_deduce_arg_index(-1), template_return_deduce_from_pointer(false), template_return_ref(false), return_typedef_name(), emit_symbol(), class_emit_name(), method_display_name(), function_display_name(), namespace_name(), ctor_trailing_self(false), is_varargs(false), is_void_params(false), no_instrument_function(false), has_large_struct_retbuf(false), declaration_only(false), defaulted_or_deleted(false), pure_virtual(false), is_const_method(false) {}
    DataDef *findParameter(std::string &);
    virtual BaseType basetype() const { return BaseType::btFunct; }
    virtual size_t alignment() const { return explicit_alignment ? explicit_alignment : DataDef::alignment(); }
    bool is_varargs;  // function declared with ... (variadic)
    bool is_void_params; // f(void) — explicitly zero params (vs f() which is K&R unspecified)
    bool no_instrument_function;
    bool has_large_struct_retbuf; // __retbuf was injected for struct return > 16 bytes
    // True when DECLARED with no body (prototype ended in ';' / ',' not '{').
    // For a C++ class method whose class carries canonical C++ spelling, this
    // can bind emit_symbol to the mangled external symbol. Stays false for any
    // madc-compiled (bodied) function.
    bool declaration_only;
    // True for C++ special declarations like `= default` or `= delete`.
    // These are not bodyless shared-library declarations and must not be bound
    // as external symbols just because the class has canonical C++ spelling.
    bool defaulted_or_deleted;
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
int score_arg_to_param(const DataDef *adc, const DataDef *pdc,
		       bool param_is_ref = false, bool allow_udc = true);

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
            // return type (the base T — FuncDef::returns strips the reference).
            // This is what lets `v[i].method()` see a structured element.
            _datatype = e;
        else
            _datatype = &ddINT64; // MadArray: default to int
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
	return &fd->returns;
    }
    virtual TokenType type() const { return TokenType::ttSubscript; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    // The element type is computed in the constructor for every container
    // kind (fixed array, pointer, string, SIMD, vector, map, MadArray).
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
    bool good() { return !_pushback.empty() || _ss.good(); }
    bool eof()  { return _pushback.empty() && _ss.eof(); }
    int line()  { if ( _lf > _cr ) return _lf+1; return _cr+1; }
    int column(){ return _column ? _column : 1; }
    int get()
    {
	if ( !_pushback.empty() )
	{
	    int ch = (unsigned char)_pushback[0];
	    _pushback.erase(0, 1);
	    bool recount = true;
	    if ( !_pushback_frames.empty() )
	    {
		recount = _pushback_frames.front().recount;
		if ( _pushback_frames.front().remaining > 0 )
		    --_pushback_frames.front().remaining;
		if ( _pushback_frames.front().remaining == 0 )
		    _pushback_frames.pop_front();
	    }
	    if ( recount )
		++_column;
	    return ch;
	}
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
    throwstream& operator()(TokenBase *t) { _tbuf.token(t); return *this; }
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
    datatype_map_t datatype_map;	// TokenDataType map
    datadef_map_t  datadef_map;		// data definitions defined by typedef or class
    datadef_map_t  struct_map;		// data definitions defined by struct
    // Captured `template<typename T> class Name {...}` definitions for
    // Borland-model instantiation: name -> {type params, the class-body token
    // range}. `Name<ConcreteT>` clones+substitutes+re-parses it as a concrete
    // class. See docs/plans/2026-05-30-template-instantiation.md.
	struct TemplateDef {
	    std::vector<std::string> typeparams;   // e.g. ["T"]
	    std::vector<std::vector<TokenBase *>> typeparam_defaults;
	    std::vector<bool> typeparam_is_type;
	    bool has_non_type_params;
	    std::string class_name;                // e.g. "Box"
	    std::vector<TokenBase *> body;         // cloned tokens: `class Name { ... }`
	std::string defining_namespace;        // current_namespace at capture (e.g. "std")
	DataDefCLASS *owner_class;             // enclosing class for member templates
	TemplateDef() : has_non_type_params(false), owner_class(nullptr) {}
    };
    std::map<std::string, TemplateDef> template_map;       // name -> definition
	struct TemplateAliasDef {
	    std::vector<std::string> typeparams;
	    std::vector<std::vector<TokenBase *>> typeparam_defaults;
	    std::vector<bool> typeparam_is_type;
	    bool has_non_type_params;
	    std::string alias_name;
	std::vector<TokenBase *> target;
	std::string defining_namespace;
	DataDefCLASS *owner_class;
	TemplateAliasDef() : has_non_type_params(false), owner_class(nullptr) {}
    };
    std::map<std::string, TemplateAliasDef> template_alias_map;
    std::vector<TokenBase *> last_skipped_template_decl;
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
    funcdef_map_t  funcdef_map;		// function definitions
    variable_map_t literal_map;		// string literals
    namespace_map_t namespace_map;	// namespace registries (std::, etc.)
    namespace_datatype_map_t namespace_datatype_map; // namespace-owned type names
    std::string current_namespace;	// active namespace for resolution (set by ns:: prefix)
    // Canonical C++ spelling of the template-id being instantiated right now,
    // stashed by instantiate_template_use around the class re-parse so
    // TokenCLASS::parse can record it on the new DataDefCLASS for bodyless C++
    // method binding.
    std::string instantiating_canonical_spelling;
    bool instantiating_dependent_surface;
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
    std::map<std::string, bool> included_files;	// #include files already tokenized (require_once semantics)
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
	const char *file;
	int line;
	int column;
	DeferredFunctionBody() : var(NULL), method(NULL), file(NULL), line(0), column(0) {}
    };
    std::vector<DeferredFunctionBody> *deferred_function_body_sink;
    bool parsing_cpp_struct_class;
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
    // tb->file undefined. Reuse the existing `included_files` map
    // (keys are std::string and stable since we never erase entries).
    const char *intern_file(const std::string &s) {
	return included_files.emplace(s, true).first->first.c_str();
    }

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
    bool is_c_mode() const { return language_std >= STD_C89 && language_std <= STD_C23; }
    bool is_cpp_mode() const { return language_std >= STD_CPP98 && language_std <= STD_CPP26; }
    bool auto_includes_enabled() const { return language_std == STD_MADC; }
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
    void add_iostream();	// populates lazy_map for cout, cin, cerr (via #include <iostream>)
    void add_stdio();		// placeholder for #include <stdio.h> registration
    Variable *lazy_resolve(const std::string &name);	// on-demand variable/function registration
    DataDef  *lazy_resolve_type(const std::string &name);	// on-demand type/struct registration
    void add_namespaces();
    void add_madc_namespace();
    bool is_namespace_registration_enabled(const std::string &name) const;
    bool is_dynamic_library_loading_enabled() const;
    bool is_dynamic_symbol_fallback_enabled() const;
    bool is_runtime_eval_source_scope_access_enabled() const;
    bool is_runtime_eval_expression_scope_access_enabled() const;
    bool is_embedded_header_allowed(const std::string &name) const;
    bool is_dynamic_symbol_allowed(const std::string &name) const;
    bool is_known_namespace(const std::string &name) const;
    Variable *runtime_eval_scope_target(Variable *var) const;
    void collect_runtime_eval_scope_variables(std::vector<Variable *> &out) const;
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
    int64_t parse_constant_ternary();
    int64_t parse_constant_integer_expression();
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
    // Type an operator expression on a class-object operand with the operator's
    // return type (Part A of generic operator-overload support). No-op unless the
    // left operand is a class object declaring the matching binary operator.
    void resolve_object_operator_type(class TokenOperator *to);
    TokenBase *parseCompound();
    TokenBase *parseStatement(TokenBase *);
    TokenBase *parseDeclaration(TokenDataType *, bool is_static = false);
    DataDefPTR *getPointerType(DataDef *base);
    // parse a `(params)` list after the opening '(' has been consumed; used by
    // function-pointer typedefs. Builds a FuncDef with the given return type.
    // Parameter names are accepted but discarded. Stops after consuming ')'.
    FuncDef *parseFnPtrParams(DataDef &returns);
    TokenBase *parseExpression(TokenBase *, bool conditional=false,
			       bool ternary_branch=false,
			       bool stop_on_closing_paren=false,
			       int initial_brackets=0,
			       bool push_back_comma=false);
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
