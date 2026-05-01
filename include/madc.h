#ifndef __MADC_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc main header file			2019 Derek Snider	//
//									//
//////////////////////////////////////////////////////////////////////////
#define __MADC_H 1

#include <istream>
#include <memory>
#include <ostream>
#include <sstream>

class Method;
class Program;
class MadcEngine;

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

class MadcAsmjitErrHandler : public asmjit::ErrorHandler
{
public:
    Program *pgm;
    int hits;
    MadcAsmjitErrHandler();
    void handleError(asmjit::Error err, const char *message,
		     asmjit::BaseEmitter *) override;
};
class FuncDef: public DataDef
{
public:
    DataDef &returns;
    asmjit::FuncNode *funcnode;
    std::vector<DataDef *> parameters;
    // [&] capture support
    bool has_captures;
    struct CaptureEntry { std::string name; DataDef *type; };
    std::vector<Variable *> potential_captures; // outer-scope vars at lambda creation time
    std::vector<CaptureEntry> captures;         // populated during lambda body compilation
    // multiple return values (empty = single return via `returns`)
    std::vector<DataDef *> return_types;
    FuncDef(DataDef &d) : returns(d), has_captures(false), is_varargs(false) { funcnode = NULL; }
    DataDef *findParameter(std::string &);
    virtual BaseType basetype() const { return BaseType::btFunct; }
    bool is_varargs;  // function declared with ... (variadic)
    bool is_multi_return() const { return return_types.size() > 1; }
};

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
    std::map<Variable *, asmjit::Operand> operand_map;
    // Stack-slot Mem for local C fixed-size arrays. Cached so reuse on a
    // divergent branch can re-emit the LEA into the (also-cached) Gp,
    // mirroring the global-fixed-array re-emit pattern. Without this,
    // the first use's LEA can sit inside a branch the second use does
    // not dominate, leaving the cached Gp uninitialized → NULL deref.
    std::map<Variable *, asmjit::x86::Mem> fixed_array_stack;
    TokenCpnd() : TokenBase() { method = NULL; parent = NULL; child = NULL; }
    virtual TokenType type() const { return TokenType::ttCompound; }
    asmjit::Operand &voperand(Program &, Variable *);
    void movreg(asmjit::x86::Compiler &, asmjit::Operand &, Variable *);
    void putreg(asmjit::x86::Compiler &, Variable *);
    void cleanup(Program &);
    void clear_operand_map() { operand_map.clear(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
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
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    // Create FuncNode (label + signature) ahead of body compilation so
    // global fn-pointer inits can LEA the label. Idempotent: skips if
    // the FuncDef already has funcnode set.
    void prepareFuncNode(Program &);
//  using TokenCpnd::getreg;
};

class TokenDecl: public TokenVar
{
public:
    TokenBase *initialize;
    std::vector<TokenBase *> init_list; // brace-enclosed initializer for fixed-size arrays
    TokenDecl(Variable &v) : TokenVar(v) { initialize = NULL; }
    virtual TokenType type() const { return TokenType::ttDeclare; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// { v0, v1, ... } — a nested brace initializer, used for elements of an
// array-of-structs or for nested struct members. Not a value by itself.
class TokenStructLit: public TokenBase
{
public:
    std::vector<TokenBase *> inits;
    TokenStructLit() {}
    virtual TokenType type() const { return TokenType::ttStructLit; }
};

class TokenCallFunc: public TokenVar
{
public:
    std::vector<TokenBase *> parameters;
    // Non-null when the function-pointer value comes from a sub-expression
    // (e.g. a struct member access c.fn or arr[i].fn) rather than a variable.
    // When set, TokenCallFunc::compile loads the fn-ptr by compiling src_node
    // instead of calling voperand(var). var.type must still be DataDefFPTR.
    TokenBase *src_node = nullptr;
    TokenCallFunc(Variable &v) : TokenVar(v) { if (v.type->is_function()) _datatype = returns(); }
    virtual DataDef *returns()  const { return &((FuncDef *)var.type)->returns; }
    virtual DataDef *datadef()  const override {
        if ( var.type->is_function() )
            return returns();
        return _datatype;
    }
    virtual size_t argc() const { return parameters.size(); }
    virtual TokenType type() const { return TokenType::ttCallFunc; }
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
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
    virtual void putreg(Program &);
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    // Member is declared as a fixed array (e.g. `SKILLTYPE *arr[N]`).
    // Such a member's datadef reports the element type but the storage
    // is in-place, so subscripting needs LEA on the member's Mem and
    // the parser must not unwrap pointer-typed elements.
    bool is_fixed_array_member() const
    {
	// For `obj->member` access, object.type is the pointer-to-struct,
	// not the struct itself. Walk through any pointer wrapper.
	DataDef *otype = object.type;
	if ( DataDefPTR *opt = dynamic_cast<DataDefPTR *>(otype) )
	    otype = opt->base_type;
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(otype);
	if ( !sdd ) return false;
	std::string mname = var.name;
	return sdd->m_count(mname) > 1;
    }
};

// & address-of operator — emits LEA to get address of a variable
class TokenAddrOf: public TokenBase
{
public:
    Variable &var;
    DataDef *ptr_type;  // pointer-to-var type
    asmjit::Operand _operand;
    TokenAddrOf(Variable &v, DataDef *pt) : var(v), ptr_type(pt) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// &(expr) address-of operator for member/subscript/deref lvalues
class TokenAddrExpr: public TokenBase
{
public:
    TokenBase *expr;
    DataDef *ptr_type;
    asmjit::Operand _operand;
    TokenAddrExpr(TokenBase *e, DataDef *pt) : expr(e), ptr_type(pt) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// *ptr dereference — reads/writes the value at the address held by a pointer
class TokenDeref: public TokenBase
{
public:
    Variable &var;
    DataDef *deref_type;  // pointed-to type
    asmjit::Operand _operand;
    TokenDeref(Variable &v, DataDef *dt) : var(v), deref_type(dt) { _datatype = dt; }
    virtual TokenType type() const { return TokenType::ttMember; }  // reuse member type for assignment compat
    virtual DataDef *datadef() const override { return deref_type; }
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// *(expr) dereference for cast/member/subscript pointer expressions
class TokenDerefExpr: public TokenBase
{
public:
    TokenBase *expr;
    DataDef *deref_type;
    asmjit::Operand _operand;
    TokenDerefExpr(TokenBase *e, DataDef *dt) : expr(e), deref_type(dt) { _datatype = dt; }
    virtual TokenType type() const { return TokenType::ttMember; }
    virtual DataDef *datadef() const override { return deref_type; }
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// *ptr++ / *ptr-- — dereference the current pointer value, then advance
// or rewind the pointer variable itself.
class TokenDerefStep: public TokenBase
{
public:
    Variable &var;
    DataDef *deref_type;
    bool increment;
    asmjit::Operand _operand;
    TokenDerefStep(Variable &v, DataDef *dt, bool inc)
        : var(v), deref_type(dt), increment(inc) { _datatype = dt; }
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return deref_type; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// (TYPE *) cast expression — type annotation, no codegen for pointer casts
class TokenCast: public TokenBase
{
public:
    DataDef *cast_type;   // target type
    TokenBase *expr;      // expression being cast
    asmjit::Operand _operand;
    TokenCast(DataDef *ct, TokenBase *e) : cast_type(ct), expr(e) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return cast_type; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenCallMethod: public TokenMember
{
public:
    TokenCallMethod(Variable &o, Variable &m) : TokenMember(o, m, 0) { _datatype = returns(); }
    virtual TokenType type() const { return TokenType::ttCallMethod; }
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// subscript access: container[index]
class TokenSubscript: public TokenBase
{
public:
    Variable &object;    // the container variable
    TokenBase *index;    // the primary (first) index expression
    std::vector<TokenBase *> extra_indices; // additional indices for multi-dim fixed arrays
    Variable *tmp_var;   // temp string variable for string-returning subscripts (or NULL)
    asmjit::Operand _operand;

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
        else if ( o.type->type() == DataType::dtVECTOR )
            _datatype = static_cast<DataDefVECTOR *>(o.type)->element_type;
        else if ( o.type->type() == DataType::dtMAP )
            _datatype = static_cast<DataDefMAP *>(o.type)->val_type;
        else
            _datatype = &ddINT64; // MadArray: default to int
    }
    virtual TokenType type() const { return TokenType::ttSubscript; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    virtual asmjit::Operand &operand(Program &pgm) {
        regdefp_t r = {nullptr, nullptr, nullptr};
        return compile(pgm, r);
    }
    virtual asmjit::Operand &compile(Program &, regdefp_t &);
    // emit setter call for write context: container[index] = val
    void compile_set(Program &, asmjit::Operand &, DataDef *);
};

class TokenSubscriptExpr: public TokenBase
{
public:
    TokenBase *base_expr;
    TokenBase *index;
    asmjit::Operand _operand;

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
    virtual asmjit::Operand &operand(Program &pgm);
    virtual asmjit::Operand &compile(Program &, regdefp_t &);
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
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
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

// map-iterators
typedef std::map<std::string, TokenKeyword *>::iterator keyword_map_iter;
typedef std::map<std::string, TokenDataType *>::iterator datatype_map_iter;
typedef std::map<std::string, DataDef *>::iterator datadef_map_iter;
typedef std::map<std::string, FuncDef *>::iterator funcdef_map_iter;
typedef std::map<std::string, Variable *>::iterator variable_map_iter;

// vectors
typedef std::vector<DataType> datatype_vec_t;
typedef std::vector<Variable *> variable_vec_t;

// vector iterators
typedef std::vector<DataDef *>::iterator datadef_vec_iter;
typedef std::vector<Variable *>::iterator variable_vec_iter;
//typedef std::vector<CodeBlock *>::iterator codeblock_vec_iter;
typedef std::vector<TokenBase *>::iterator tokenbase_vec_iter;

typedef std::pair<asmjit::Label *, asmjit::Label *> l_shortcut_t;
typedef std::stack<l_shortcut_t> shortstack_t;


// class to hold source for lexing
// embedded header lookup (generated by scripts/gen_embedded_headers.sh)
const std::string *find_embedded_header(const std::string &name);

class Source
{
protected:
    std::stringstream _ss;
    std::string _pushback;		// pushback buffer for #define substitution
    int _lf, _cr, _column;
    std::streampos _pos;
    std::string _fname;
public:
    Source() { _lf = 0; _cr = 0; _column = 0; _pos = 0; }
    const char *fname() const { return _fname.c_str(); }
    const char *fname(const char *s)  { _fname = s; return _fname.c_str(); }
    const char *fname(std::string &s) { _fname = s; return _fname.c_str(); }
    void copybuf(std::streambuf *sb)  { _ss << sb;  }
    void str(const std::string &s) { _ss.str(s); }
    void pushback(const std::string &s) { _pushback = s + _pushback; }
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
	    ++_column;
	    return ch;
	}
	int ch = _ss.get();
	if ( ch == -1 ) { return -1; }
	/**/ if ( ch == '\n' ) { ++_lf; _column = 0; _pos = _ss.tellg(); }
	else if ( ch == '\r' ) { ++_cr; _column = 0; _pos = _ss.tellg(); }
	else { ++_column; }
	return ch;
    }
    int peek()
    {
	if ( !_pushback.empty() )
	    return (unsigned char)_pushback[0];
	return _ss.peek();
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
    asmjit::x86::Mem __const_double_1;	// const double of 1.0
public:
    MadcEngine *engine;
    std::istream *input_stream;
    std::ostream *output_stream;
    std::ostream *error_stream;
    RegistrationPolicy registration_policy;
    BuiltinRegistry builtin_registry;
    NamespaceRegistry namespace_registry;
    ErrorInfo last_error;
    std::vector<Diagnostic> diagnostics;
    keyword_map_t  keyword_map;		// reserved keywords
    datatype_map_t datatype_map;	// TokenDataType map
    datadef_map_t  datadef_map;		// data definitions defined by typedef or class
    datadef_map_t  struct_map;		// data definitions defined by struct
    std::map<DataDef*, DataDefPTR*> ptr_type_cache; // cached pointer-to-T DataDefs
    funcdef_map_t  funcdef_map;		// function definitions
    variable_map_t literal_map;		// string literals
    namespace_map_t namespace_map;	// namespace registries (std::, etc.)
    std::string current_namespace;	// active namespace for resolution (set by ns:: prefix)
    std::vector<std::string> namespace_preference; // ordered namespace lookup; "c" means normal lexical/global resolution
    std::map<std::string, void *> dlopen_map;	// dlopen handles for loaded libraries
    // function-like macro definitions: #define NAME(params) body
    struct MacroDef {
	std::vector<std::string> params;  // parameter names
	bool variadic = false;           // trailing ... / __VA_ARGS__
	std::string body;                 // body template with param names as placeholders
    };
    std::map<std::string, MacroDef> macro_map;	// function-like macros
    enum LazyKind { lkVariable = 1, lkFunction = 2, lkType = 3, lkStruct = 4 };
    struct LazyEntry { int header; LazyKind kind; };
    std::map<std::string, LazyEntry> lazy_map;	// deferred symbol registration
    std::map<std::string, std::string> define_map;	// #define name value
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
    std::stack<TokenCpnd *> compounds;	// stack to manage nested brackets
    std::stack<l_shortcut_t> loopstack;	// stack to manage break/continue for loops
    std::stack<l_shortcut_t> ifstack;	// stack to manage short circuit boolean for if/else
    TokenProgram *tkProgram;		// program token
    TokenCpnd *tkFunction;		// function we are currently in
    std::string cur_func_name;		// name of current function being compiled (for diagnostics)
    throwstream Throw;			// throw an error
    int script_argc;			// argc for the .mad script
    char **script_argv;			// argv for the .mad script
    bool _include_iostream;		// #include <iostream> was seen during tokenization
    bool _include_stdio;		// #include <stdio.h> was seen during tokenization
    // Intern file paths so TokenBase::file pointers stay stable for
    // the program's lifetime. Lexer used to store `c_str()` of a
    // stack-local std::string into tokens — the pointer dangled the
    // moment the include scope ended, leaving every later read of
    // tb->file undefined. Reuse the existing `included_files` map
    // (keys are std::string and stable since we never erase entries).
    const char *intern_file(const std::string &s) {
	return included_files.emplace(s, true).first->first.c_str();
    }

    bool parsing_extern_decl = false;	// current declaration originated from `extern`
    bool parsing_static_decl = false;	// current declaration originated from `static` (propagates through `static struct X x;` path so parseDeclaration knows to allocate persistent storage)

    // JIT crash → source location map. Populated during compile by
    // record_compile_anchor() and finalized into jit_source_map after
    // cc.finalize(). The signal handler binary-searches jit_source_map
    // by faulting RIP - root_fn to print the .mad source line that
    // emitted the crashing instruction. Disable with MADC_NO_SOURCE_MAP=1
    // env var (small overhead per anchor: one Label + entry per
    // top-level / per-statement compile call).
    struct JitSourceEntry
    {
	uint32_t byte_offset;
	const char *file;
	uint32_t line;
	uint16_t col;
	const char *kind;	// short token-type label (e.g. "stmt", "fn")
    };
    bool jit_source_map_enabled = true;
    std::vector<std::pair<asmjit::Label, JitSourceEntry>> jit_anchor_labels;
    std::vector<JitSourceEntry> jit_source_map;
    MadcAsmjitErrHandler asmjit_err_handler;
    void record_compile_anchor(class TokenBase *tb, const char *kind);
    std::stack<int> _pack_stack;	// #pragma pack(push, N) / pop stack
    int pack_stack_top() { return _pack_stack.empty() ? 0 : _pack_stack.top(); }

    // goto / label map — function-scoped. `TokenFunc::compile` clears
    // this at the start of each function body; `TokenGOTO::compile`
    // look-or-creates entries; `TokenLabel::compile` binds them.
    // Kept on Program rather than TokenFunc to avoid a layout change
    // in TokenFunc's multi-inheritance shape (which silently regressed
    // downstream codegen when an `std::map` was added there directly).
    std::map<std::string, asmjit::Label> label_map;

    bool colors;
    asmjit::JitRuntime jit;
    asmjit::CodeHolder code;
    asmjit::x86::Compiler cc;
    fVOIDFUNC root_fn;

    Program();
    explicit Program(MadcEngine *eng);
    void attach_engine(MadcEngine *eng);

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
    void add_string_methods();
    void add_sstream_methods();
    void add_fstream_methods();
    void populate_builtin_registry();
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
    void add_php_namespace();
    void add_perl_namespace();
    void add_python_namespace();
    void add_ruby_namespace();
    void add_js_namespace();
    void add_rust_namespace();
    bool is_namespace_registration_enabled(const std::string &name) const;
    bool is_known_namespace(const std::string &name) const;
    void set_namespace_preference(const std::vector<std::string> &order, TokenBase *tb = NULL);
    Variable *find_namespace_member(const std::string &ns_name, const std::string &member_name);
    Variable *resolve_preferred_identifier(class TokenIdent *ident_tb, bool expression_head);
    std::string current_source_directory();
    bool include_already_seen(const std::string &path);
    std::string resolve_include_path(const std::string &incfile, bool is_system);
    bool should_tokenize_include(const std::string &path);

    Variable *addFunction(std::string, datatype_vec_t, fVOIDFUNC, bool isMethod=false);

    // manage compound nesting
    void pushCompound();
    void popCompound();

    // generate tokens
    TokenBase *getToken();
    TokenBase *getRealToken();
//  TokenProgram *tokenize(std::istream &);
    TokenProgram *tokenize(const char *);
    // C/C++ translation phase 6: adjacent string literals concatenate.
    // Funnel every tokens.push_back through this helper so an
    // included `SYSTEM_DIR "file.dat"` (= `"../system/" "file.dat"`)
    // ends up as one merged literal, not two adjacent tokens whose
    // first one gets dropped by parser exStack semantics.
    void push_token_with_string_concat(TokenBase *tb);

    // for debugging
    void printt(TokenBase *);
//  void showerror(std::istream &);

    // accessing token queue
    inline TokenBase *peekToken() { if (tokens.empty()) return NULL; return tokens.front(); }
    inline TokenBase *prevToken() { return _prv_token; }
    inline TokenBase *curToken()  { return _cur_token; }
    inline void resetPrevToken() { _prv_token = NULL; }
    inline void pushToken(TokenBase *t) { tokens.push_front(t); }

    // helper: is prevToken in a position where the next operator would be unary?
    // true when prevToken is NULL, ;, {, (, ,, =, or any operator except ) and ]
    inline bool isUnaryPosition()
    {
	if ( !_prv_token ) return true;
	TokenID id = _prv_token->id();
	if ( id == TokenID::tkSemi || id == TokenID::tkOpBrc
	||   id == TokenID::tkOpBrk || id == TokenID::tkComma
	||   id == TokenID::tkAssign ) return true;
	if ( _prv_token->is_operator()
	&&   id != TokenID::tkClBrk && id != TokenID::tkClSqr ) return true;
	// Keywords like `return`, `if`, `while`, `case` open an expression
	// context — the following `-` should be unary negation, not binary
	// subtraction with a missing left operand.
	if ( _prv_token->type() == TokenType::ttKeyword ) return true;
	return false;
    }
    // helper: is prevToken in a position where the next operator would be postfix?
    // true when prevToken is ), ], or a non-operator value token
    inline bool isPostfixPosition()
    {
	if ( !_prv_token ) return false;
	TokenID id = _prv_token->id();
	// Keywords aren't values — they open expression contexts, not close them.
	if ( _prv_token->type() == TokenType::ttKeyword ) return false;
	// Symbols that open or continue expression contexts aren't values
	// either — `{`, `(`, `,`, `;`, `=` mean the next `-` / `!` is
	// unary, not binary. Without this guard `int x[] = { -5 };`
	// converted TokenNeg → TokenSub at the unary-after-`{` slot and
	// emitted a binary subtraction missing its left operand.
	if ( id == TokenID::tkOpBrc || id == TokenID::tkOpBrk
	||   id == TokenID::tkComma || id == TokenID::tkSemi
	||   id == TokenID::tkAssign )
	    return false;
	return id == TokenID::tkClBrk || id == TokenID::tkClSqr || !_prv_token->is_operator();
    }
    inline TokenBase *nextToken()
    {
	if ( tokens.empty() )
	    throw "Unexpected end of data";
        _prv_token = _cur_token;
	_cur_token = tokens.front();
//	DBG(cout << "nextToken(" << (int)ret->type() << ", " << (int)ret->id() << ')' << endl);
	tokens.pop_front();
	return _cur_token;
    }
    // parse tokens into AST
    bool load_file(const char *fname);
    bool parse(TokenProgram *);
    void parseIdentifier(TokenIdent *);
    void parseFunction(DataDef &, std::string &, DataDefCLASS *owner_class = NULL,
		       std::vector<DataDef *> *multi_ret = NULL);
    TokenBase *parseKeyword(TokenKeyword *);
    TokenBase *parseCallFunc(TokenCallFunc *);
    TokenBase *parseCallMethod(TokenCallMethod *);
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
    TokenBase *parseLambda();  // parse [](params) { body } lambda expression

    // perform cc.mov with size casting
    void safemov(asmjit::x86::Gp &,  asmjit::x86::Gp &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Gp &,  asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Gp &,  asmjit::x86::Mem &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Xmm &, asmjit::x86::Gp &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Xmm &, asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Xmm &, asmjit::x86::Mem &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Mem &, asmjit::x86::Gp &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Mem &, asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safemov(asmjit::Operand &,  asmjit::Operand &, DataDef *d1=NULL, DataDef *d2=NULL);
    // int, int64 and double are the standard numeric token types
    void safemov(asmjit::Operand &,  int, DataDef *d1=NULL, DataDef *d2=NULL);
    void safemov(asmjit::Operand &,  int64_t, DataDef *d1=NULL, DataDef *d2=NULL);
    void safemov(asmjit::Operand &,  double, DataDef *d1=NULL, DataDef *d2=NULL);

    // perform cc.add with size casting
    void safeadd(asmjit::x86::Gp &,  asmjit::x86::Gp &, DataDef *, DataDef *);
    void safeadd(asmjit::x86::Xmm &, asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safeadd(asmjit::Operand &,  asmjit::Operand &, DataDef *d1=NULL, DataDef *d2=NULL);
    void safeadd(asmjit::Operand &,  int, DataDef *, DataDef *);

    // perform cc.sub with size casting
    void safesub(asmjit::x86::Gp &,  asmjit::x86::Gp &, DataDef *, DataDef *);
    void safesub(asmjit::x86::Xmm &, asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safesub(asmjit::Operand &,  asmjit::Operand &, DataDef *d1=NULL, DataDef *d2=NULL);
    void safesub(asmjit::Operand &,  int, DataDef *, DataDef *);

    // perform cc.mul with size casting
    void safemul(asmjit::Operand &,  asmjit::Operand &, DataDef *d1=NULL, DataDef *d2=NULL);
    // perform cc.div with size casting
    void safediv(asmjit::Operand &,  asmjit::Operand &,  asmjit::Operand &, DataDef *d1=NULL, DataDef *d2=NULL, DataDef *d3=NULL);
    // perform cc.shl with size casting
    void safeshl(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.shr with size casting
    void safeshr(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.or_ with size casting
    void safeor(asmjit::Operand &,   asmjit::Operand &);
    // perform cc.and_ with size casting
    void safeand(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.xor_ with size casting
    void safexor(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.not_ with size casting
    void safenot(asmjit::Operand &);

    // perform cc.inc with size casting
    void safeinc(asmjit::Operand &);
    // perform cc.dec with size casting
    void safedec(asmjit::Operand &);

    // negate the operand
    void safeneg(asmjit::Operand &);

    // return the operand
    void saferet(asmjit::Operand &);

    // perform cc.test with size casting
    void safetest(asmjit::Operand &, asmjit::Operand &);

    // tests if operand is zero
    void testzero(asmjit::Operand &);

    // sign/zero extend operand in place
    void safeextend(asmjit::Operand &, bool unsign=false);

    // perform cc.setCC with size casting
    void safesete(asmjit::Operand &);
    void safesetg(asmjit::Operand &);
    void safesetge(asmjit::Operand &);
    void safesetl(asmjit::Operand &);
    void safesetle(asmjit::Operand &);
    void safesetne(asmjit::Operand &);
    // Unsigned variants — use when either comparison operand is unsigned.
    // C's "usual arithmetic conversions" treat mixed unsigned/signed as
    // unsigned, so signed setl/setg give wrong results when compared
    // against values whose signed interpretation flips (e.g. unsigned
    // short cmp vs 65535 → signed-interpret as -1 → bogus ordering).
    void safesetb(asmjit::Operand &);   // below          (unsigned <)
    void safesetbe(asmjit::Operand &);  // below-or-equal (unsigned <=)
    void safeseta(asmjit::Operand &);   // above          (unsigned >)
    void safesetae(asmjit::Operand &);  // above-or-equal (unsigned >=)

    // perform cc.cmp with size casting
    void safecmp(asmjit::x86::Gp &,  asmjit::x86::Gp &);
    void safecmp(asmjit::x86::Xmm &, asmjit::x86::Xmm &);
    void safecmp(asmjit::Operand &,  asmjit::Operand &);

    // compile code
    bool compile();

    // execute the resulting code
    void execute();

    // data management
    DataDef *findType(std::string &);
    Variable *addVariable(TokenCpnd *, DataDef &, std::string &, int c=1, void *init=NULL, bool alloc=true);
    Variable *addGlobal(DataDef &d, std::string str, int c=1, void *init=NULL)
    {
	return addVariable(NULL, d, str, c, init, true);
    }
    Variable *findVariable(TokenCpnd *, std::string &);
    Variable *findVariable(std::string &);
    Variable *addLiteral(std::string &);
//  Method *findMethod(std::string &);
};

class MadcEngine
{
public:
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
    Program::RegistrationPolicy registration_policy;
    Program::BuiltinRegistry builtin_registry;
    Program::NamespaceRegistry namespace_registry;

    MadcEngine();
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
};

#define ANSI_RED "\e[1;31m"
#define ANSI_WHITE "\e[1;37m"
#define ANSI_RESET "\e[m"

#endif // __MADC_H
