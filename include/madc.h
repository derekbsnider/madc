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
    std::map<Variable *, asmjit::Operand> operand_map;
    TokenCpnd() : TokenBase() { method = NULL; parent = NULL; child = NULL; }
    virtual TokenType type() const { return TokenType::ttCompound; }
    asmjit::Operand &voperand(Program &, Variable *);
    void movreg(asmjit::x86::Compiler &, asmjit::Operand &, Variable *);
    void putreg(asmjit::x86::Compiler &, Variable *);
    void cleanup(asmjit::x86::Compiler &);
    void clear_operand_map() { operand_map.clear(); }
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
//  using TokenCpnd::getreg;
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
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenMember: public TokenVar
{
public:
    Variable &object;
    size_t offset;
    TokenMember(Variable &o, Variable &m, size_t ofs) : TokenVar(m), object(o), offset(ofs) { _datatype = m.type; }
    virtual TokenType type() const { return TokenType::ttMember; }
    virtual bool is_real() { return _datatype->is_real(); }
    virtual void putreg(Program &);
    virtual asmjit::Operand &operand(Program &);
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
    virtual bool is_real() { return _datatype->is_real(); }
//  virtual asmjit::Operand &operand(Program &);
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

// class to hold source for lexing
class Source
{
protected:
    std::stringstream _ss;
    int _lf, _cr, _column;
    std::streampos _pos;
public:
    Source() { _lf = 0; _cr = 0; _column = 0; _pos = 0; }
    void copybuf(std::streambuf *sb) { _ss << sb; }
    void str(const std::string &s) { _ss.str(s); }
    bool good() { return _ss.good(); }
    bool eof()  { return _ss.eof(); }
    int line()  { if ( _lf > _cr ) return _lf+1; return _cr+1; }
    int column(){ return _column; }
    int get()
    {
	int ch = _ss.get();
	if ( ch == -1 ) { return -1; }
	/**/ if ( ch == '\n' ) { ++_lf; _column = 0; _pos = _ss.tellg(); }
	else if ( ch == '\r' ) { ++_cr; _column = 0; _pos = _ss.tellg(); }
	else { ++_column; }
	return ch;
    }
    int peek()
    {
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
    void showerror(int row=0, int col=0);
};


// program class, keep things somewhat contained
class Program
{
protected:
    TokenBase *_getToken();
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
    TokenBase *getToken();
    TokenBase *getRealToken();
//  TokenProgram *tokenize(std::istream &);
    TokenProgram *tokenize(const char *);

    // for debugging
    void printt(TokenBase *);
//  void showerror(std::istream &);

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
    void safemov(asmjit::x86::Gp &,  asmjit::x86::Gp &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Gp &,  asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Gp &,  asmjit::x86::Mem &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Xmm &, asmjit::x86::Gp &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Xmm &, asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Xmm &, asmjit::x86::Mem &, DataDef *, DataDef *);
    void safemov(asmjit::x86::Xmm &, asmjit::Imm &, DataDef *, DataDef *);
    void safemov(asmjit::Operand &,  asmjit::Operand &, DataDef *d1=NULL, DataDef *d2=NULL);
    // only int and double are standard numeric token types
    void safemov(asmjit::Operand &,  int, DataDef *d1=NULL, DataDef *d2=NULL);
    void safemov(asmjit::Operand &,  double, DataDef *d1=NULL, DataDef *d2=NULL);

    // perform cc.add with size casting
    void safeadd(asmjit::x86::Gp &,  asmjit::x86::Gp &, DataDef *, DataDef *);
    void safeadd(asmjit::x86::Gp &,  asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safeadd(asmjit::x86::Xmm &, asmjit::x86::Gp &, DataDef *, DataDef *);
    void safeadd(asmjit::x86::Xmm &, asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safeadd(asmjit::x86::Xmm &, asmjit::Imm &, DataDef *, DataDef *);
    void safeadd(asmjit::Operand &,  asmjit::Operand &, DataDef *d1=NULL, DataDef *d2=NULL);
    void safeadd(asmjit::Operand &,  int, DataDef *, DataDef *);

    // perform cc.sub with size casting
    void safesub(asmjit::x86::Gp &,  asmjit::x86::Gp &, DataDef *, DataDef *);
    void safesub(asmjit::x86::Gp &,  asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safesub(asmjit::x86::Xmm &, asmjit::x86::Gp &, DataDef *, DataDef *);
    void safesub(asmjit::x86::Xmm &, asmjit::x86::Xmm &, DataDef *, DataDef *);
    void safesub(asmjit::x86::Xmm &, asmjit::Imm &, DataDef *, DataDef *);
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

    // perform cc.cmp with size casting
    void safecmp(asmjit::x86::Gp &,  asmjit::x86::Gp &);
    void safecmp(asmjit::x86::Gp &,  asmjit::x86::Xmm &);
    void safecmp(asmjit::x86::Xmm &, asmjit::x86::Gp &);
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

#define ANSI_RED "\e[1;31m"
#define ANSI_WHITE "\e[1;37m"
#define ANSI_RESET "\e[m"

#endif // __MADC_H
