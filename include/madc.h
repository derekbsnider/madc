#ifndef __MADC_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc main header file			2019 Derek Snider	//
//									//
//////////////////////////////////////////////////////////////////////////
#define __MADC_H 1

class Method;

class FuncDef: public DataDef
{
public:
    DataDef &returns;
    asmjit::FuncNode *funcnode;
    std::vector<DataDef *> parameters;
    FuncDef(DataDef &d) : returns(d) { funcnode = NULL; }
    DataDef *findParameter(std::string &);
    virtual BaseType basetype() const { return BaseType::btFunct; }
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
    Method(Variable &v) : returns(v) { x86code = NULL; }
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
    std::map<Variable *, asmjit::x86::Gp> register_map;
    TokenCpnd() : TokenBase() { method = NULL; parent = NULL; child = NULL; }
    virtual TokenType type() const { return TokenType::ttCompound; }
    asmjit::x86::Gp &getvreg(asmjit::x86::Compiler &, Variable *);
    void movreg(asmjit::x86::Compiler &, asmjit::x86::Gp &, Variable *);
    void putreg(asmjit::x86::Compiler &, Variable *);
    void cleanup(asmjit::x86::Compiler &);
    void clear_regmap() { register_map.clear(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    Variable *getParameter(unsigned int);
    Variable *findParameter(std::string &s);
    Variable *findVariable(std::string &);
};

class TokenFunc: public TokenVar, public TokenCpnd
{
public:
    TokenFunc(Variable &v) : TokenVar(v), TokenCpnd() {}
    virtual size_t argc() const { if (var.type->basetype() != BaseType::btFunct) return 0; return ((FuncDef *)var.type)->parameters.size(); }
    virtual TokenType type() const { return TokenType::ttFunction; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    using TokenCpnd::getreg;
};

class TokenDecl: public TokenVar
{
public:
    TokenBase *initialize;
    TokenDecl(Variable &v) : TokenVar(v) { initialize = NULL; }
    virtual TokenType type() const { return TokenType::ttDeclare; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenCallFunc: public TokenVar
{
public:
    std::vector<TokenBase *> parameters;
    TokenCallFunc(Variable &v) : TokenVar(v) {}
    virtual DataDef *returns()  { return &((FuncDef *)var.type)->returns; }
    virtual size_t argc() const { return parameters.size(); }
    virtual TokenType type() const { return TokenType::ttCallFunc; }
    virtual asmjit::x86::Gp &getreg(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenMember: public TokenVar
{
public:
    Variable &object;
    size_t offset;
    TokenMember(Variable &o, Variable &m, size_t ofs) : TokenVar(m), object(o), offset(ofs) { _datatype = m.type; }
    virtual TokenType type() const { return TokenType::ttMember; }
    virtual asmjit::x86::Gp &getreg(Program &);
    virtual void putreg(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenCallMethod: public TokenMember
{
public:
    std::vector<TokenBase *> parameters;
    TokenCallMethod(Variable &o, Variable &m) : TokenMember(o, m, 0) { _datatype = returns(); }
    virtual DataDef *returns()  { return &((FuncDef *)var.type)->returns; }
    virtual size_t argc() const { return parameters.size(); }
    virtual TokenType type() const { return TokenType::ttCallMethod; }
//  virtual asmjit::x86::Gp &getreg(Program &);
//  virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
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

// program class, keep things somewhat contained
class Program
{
protected:
    TokenBase *_getToken(std::istream &);
    void popOperator(std::stack<TokenBase *> &, std::stack<TokenBase *> &);
    inline int get(std::istream &is) { ++_column; return is.get(); }
    // initializers / finalizers
    void _tokenizer_init();
    void _parser_init();
    void _compiler_init();
    bool _compiler_finalize();

    int _line, _column, _braces;
    std::streampos _pos;
    TokenBase *_prv_token;
    TokenBase *_cur_token;
public:
    keyword_map_t  keyword_map;		// reserved keywords
    datatype_map_t datatype_map;	// TokenDataType map
    datadef_map_t  datadef_map;		// data definitions defined by typedef or class
    datadef_map_t  struct_map;		// data definitions defined by struct
    funcdef_map_t  funcdef_map;		// function definitions
    variable_map_t literal_map;		// string literals
    std::queue<TokenBase *> ast;	// Abstract Syntax Tree
    std::queue<TokenBase *> tokens;	// parsed token queue
    std::stack<TokenCpnd *> compounds;	// stack to manage nested brackets
    std::stack<l_shortcut_t> loopstack;	// stack to manage break/continue for loops
    std::stack<l_shortcut_t> ifstack;	// stack to manage short circuit boolean for if/else
    TokenProgram *tkProgram;		// program token
    TokenCpnd *tkFunction;		// function we are currently in

    bool colors;
    asmjit::JitRuntime jit;
    asmjit::CodeHolder code;
    asmjit::x86::Compiler cc;
    fVOIDFUNC root_fn;

    void add_keywords();
    void add_datatypes();
    void add_string_methods();
    void add_functions();
    void add_globals();

    Variable *addFunction(std::string, datatype_vec_t, fVOIDFUNC, bool isMethod=false);

    // manage compound nesting
    void pushCompound();
    void popCompound();

    // generate tokens
    TokenBase *getToken(std::istream &);
    TokenBase *getRealToken(std::istream &);
//  TokenProgram *tokenize(std::istream &);
    TokenProgram *tokenize(const char *);

    // for debugging
    void printt(TokenBase *);
    void showerror(std::istream &);

    // accessing token queue
    inline TokenBase *peekToken() { if (tokens.empty()) return NULL; return tokens.front(); }
    inline TokenBase *prevToken() { return _prv_token; }
    inline TokenBase *nextToken()
    {
	if ( tokens.empty() )
	    throw "Unexpected end of data";
        _prv_token = _cur_token;
	_cur_token = tokens.front();
//	DBG(cout << "nextToken(" << (int)ret->type() << ", " << (int)ret->id() << ')' << endl);
	tokens.pop();
	return _cur_token;
    }
    // parse tokens into AST
    bool parse(TokenProgram *);
    void parseIdentifier(TokenIdent *);
    void parseFunction(DataDef &, std::string &);
    TokenBase *parseKeyword(TokenKeyword *);
    TokenBase *parseCallFunc(TokenCallFunc *);
    TokenBase *parseCompound();
    TokenBase *parseStatement(TokenBase *);
    TokenBase *parseDeclaration(TokenDataType *);
    TokenBase *parseExpression(TokenBase *, bool conditional=false);

    // perform cc.mov with size casting
    inline void safemov(asmjit::x86::Gp &,  asmjit::x86::Gp &);
    inline void safemov(asmjit::x86::Gp &,  asmjit::x86::Xmm &);
    inline void safemov(asmjit::x86::Xmm &, asmjit::x86::Gp &);
    inline void safemov(asmjit::x86::Xmm &, asmjit::x86::Xmm &);
    inline void safemov(asmjit::x86::Xmm &, asmjit::Imm &);
    inline void safemov(asmjit::Operand &,  asmjit::Operand &);
    inline void safemov(asmjit::Operand &,  int);

    // perform cc.add with size casting
    inline void safeadd(asmjit::x86::Gp &,  asmjit::x86::Gp &);
    inline void safeadd(asmjit::x86::Gp &,  asmjit::x86::Xmm &);
    inline void safeadd(asmjit::x86::Xmm &, asmjit::x86::Gp &);
    inline void safeadd(asmjit::x86::Xmm &, asmjit::x86::Xmm &);
    inline void safeadd(asmjit::x86::Xmm &, asmjit::Imm &);
    inline void safeadd(asmjit::Operand &,  asmjit::Operand &);
    inline void safeadd(asmjit::Operand &,  int);

    // perform cc.sub with size casting
    inline void safesub(asmjit::x86::Gp &,  asmjit::x86::Gp &);
    inline void safesub(asmjit::x86::Gp &,  asmjit::x86::Xmm &);
    inline void safesub(asmjit::x86::Xmm &, asmjit::x86::Gp &);
    inline void safesub(asmjit::x86::Xmm &, asmjit::x86::Xmm &);
    inline void safesub(asmjit::x86::Xmm &, asmjit::Imm &);
    inline void safesub(asmjit::Operand &,  asmjit::Operand &);
    inline void safesub(asmjit::Operand &,  int);

    // perform cc.mul with size casting
    inline void safemul(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.div with size casting
    inline void safediv(asmjit::Operand &,  asmjit::Operand &,  asmjit::Operand &);
    // perform cc.shl with size casting
    inline void safeshl(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.shr with size casting
    inline void safeshr(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.or_ with size casting
    inline void safeor(asmjit::Operand &,   asmjit::Operand &);
    // perform cc.and_ with size casting
    inline void safeand(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.xor_ with size casting
    inline void safexor(asmjit::Operand &,  asmjit::Operand &);
    // perform cc.not_ with size casting
    inline void safenot(asmjit::Operand &);

    // negate the operand
    inline void safeneg(asmjit::Operand &);

    // return the operand
    inline void saferet(asmjit::Operand &);

    // perform cc.test with size casting
    inline void safetest(asmjit::Operand &, asmjit::Operand &);

    // perform cc.setCC with size casting
    inline void safesete(asmjit::Operand &);
    inline void safesetg(asmjit::Operand &);
    inline void safesetge(asmjit::Operand &);
    inline void safesetl(asmjit::Operand &);
    inline void safesetle(asmjit::Operand &);
    inline void safesetne(asmjit::Operand &);

    // perform cc.cmp with size casting
    inline void safecmp(asmjit::x86::Gp &,  asmjit::x86::Gp &);
    inline void safecmp(asmjit::x86::Gp &,  asmjit::x86::Xmm &);
    inline void safecmp(asmjit::x86::Xmm &, asmjit::x86::Gp &);
    inline void safecmp(asmjit::x86::Xmm &, asmjit::x86::Xmm &);
    inline void safecmp(asmjit::Operand &,  asmjit::Operand &);

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

#define ANSI_RED "\e[1;31m"
#define ANSI_WHITE "\e[1;37m"
#define ANSI_RESET "\e[m"

#endif // __MADC_H
