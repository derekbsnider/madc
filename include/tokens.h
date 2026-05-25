#ifndef __TOKENS_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Tokens								//
//									//
//////////////////////////////////////////////////////////////////////////
#define __TOKENS_H 1

// forward declaration
class Program;
//class DataDef;

//typedef std::pair<asmjit::x86::Gp *, DataDef *> regdefp_t;
typedef struct { asmjit::Operand *first; DataDef *second; asmjit::Operand *object; } regdefp_t;

enum class TokenType { 
//	0	1	 2	3	4	  5		6	  7
	ttBase, ttSpace, ttTab, ttEOL, ttComment, ttOperator, ttMultiOp, ttSymbol,
//	8		9	  10	 11	    12	    13		14	    15
	ttIdentifier, ttString, ttChar, ttInteger, ttReal, ttKeyword, ttDataType, ttVariable,
//	16		17	  18		19	  20		21	22	 23
	ttFunction, ttCallFunc, ttStatement, ttCompound, ttDeclare, ttProgram, ttMember, ttCallMethod, ttSubscript,
	ttStructLit
};

enum class TokenID {
// 0	  1	   2	  3	 4	5	6	  7	    8	   9	   9		 10	11
  tkBase, tkSpace, tkTab, tkEOL, tkREM, tkHash, tkAssign, tkEquals, tk3Eq, tkPlus, tkAdd=tkPlus, tkInc, tkSub,
// 11		12	13	13		14	14		15	16	17	18	19
  tkDash=tkSub, tkDec, tkMul, tkStar=tkMul, tkSlash, tkDiv=tkSlash, tkBslsh, tkOpBrc, tkClBrc, tkOpBrk, tkClBrk,
// 20	   21	    22	   23	   24	   25	  26	 27	28	29	29		30	31
  tkOpSqr, tkClSqr, tkNeg, tkNot, tkBand, tkLand, tkBor, tkLor, tkXor, tkMod, tkQmark, tkTerQ=tkQmark, tkColon,
// 31		  32	33	34	 35	36	 37	  38	   39	 40	41	42	43
  tkTerC=tkColon, tkNS, tkSemi, tkComma, tkDot, tkDeRef, tkQuote, tkApost, tkGT, tkLT, tkBSL, tkBSR, tkAddEq,
// 44	   45	    46		47	48	49	50	  51	52	53	54	55	56
  tkBSLEq, tkBSREq, tkBandEq, tkBnot, tkBorEq, tkDivEq, tkFuncOp, tkGE, tkLE, tkLnot, tkModEq, tkMulEq, tk3Way,
// 57	   58		59	60	61	62	63	64	65		66	67
  tkNotEq, tkSubEq, tkXorEq, tkIdent, tkInt, tkChar, tkStr, tkOperator, tkDeclare, tkArrayOp, tkMultiOp, tkReal,
  tkColEq,  // := short variable declaration (Go-style)
// keywords
// 68	69	70	71	72	73	74	75	76	77	78	79
  tkDO, tkIF, tkFOR, tkELSE, tkRETURN, tkGOTO, tkCASE, tkBREAK, tkCONT, tkTRY, tkCATCH, tkTHROW,
// 80		81	82	83	84		85	86
  tkSWITCH, tkWHILE, tkCLASS, tkSTRUCT, tkDEFAULT, tkTYPEDEF, tkOPEROVER, tkREGISTER,
  tkUSING, tkNAMESPACE, tkPREFER, tkDEFER, tkSTATIC, tkCONST, tkEXTERN, tkENUM, tkRESTRICT, tkVOLATILE,
  tkVECTOR, tkMAP, tkSET, tkLIST,
  tkFatArrow, tkMATCH,    // => (rust::match arm) and the match statement itself
  tkUNION, tkNEW, tkDELETE
};

enum class TokenAssoc {
    taNone, taLeftToRight, taRightToLeft
};


// Token flags
typedef enum : uint16_t { tfBRACKETED	=    1,
			  tfOVERLOADED  =    2,
			} tokflag_t;

class TokenBase
{
protected:
    int64_t _token;
    DataDef *_datatype;
    asmjit::x86::Gp _reg;
    asmjit::Operand _operand;
    uint16_t _flags;
public:
    const char *file;
    TokenBase *parent;
    int line;
    int column;
    std::streampos pos;
    // Current parse position — updated by nextToken(), inherited by
    // all new tokens so synthetic parser-created tokens automatically
    // get the position of the most recently consumed source token.
    static const char *_parse_file;
    static int _parse_line;
    static int _parse_column;
    TokenBase()           { _token = 0; _datatype = &ddVOID; _flags = 0; file = _parse_file; parent = NULL; line = _parse_line; column = _parse_column; pos = 0; }
    TokenBase(int64_t t)  { _token = t; _datatype = &ddVOID; _flags = 0; file = _parse_file; parent = NULL; line = _parse_line; column = _parse_column; pos = 0; }
    virtual ~TokenBase() {}
    virtual TokenBase *clone() { return new TokenBase(_token); }
    virtual void set(int64_t c) { _token = c; }
    virtual void setDataType(DataDef *d) { if (d) _datatype = d; }
    virtual void setFlag(tokflag_t f) { _flags |= f; }
    virtual bool is_bracketed() const { return (_flags & tfBRACKETED) ? true : false;  }
    virtual bool is_overloaded() const { return (_flags & tfOVERLOADED) ? true : false; }
    virtual bool is_operator() const { return false; }
    virtual bool is_constant() const { return false; }
    virtual bool is_real()     const { return false; }
    virtual int inc() { return 0; }
    virtual int dec() { return 0; }
    virtual int64_t get() const  { return _token; }
    virtual int64_t ival() const { return 0; }
    virtual double dval() const { return 0; }
    virtual size_t argc() const { return 0; }
    virtual TokenType  type()  const { return TokenType::ttBase; }
    virtual TokenID    id()    const { return TokenID::tkBase; }
    virtual DataType datatype() const { return _datatype ? _datatype->type() : DataType::dtVOID; }
    virtual DataDef *datadef()  const { return _datatype ? _datatype : &ddVOID; }
    virtual TokenAssoc associativity() const { return TokenAssoc::taNone; }
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// whitespace

// plain space
class TokenSpace: public TokenBase
{
public:
    int cnt;
    TokenSpace() : TokenBase(' ') { cnt = 0; }
    TokenSpace(int c) : TokenBase(' ') { cnt = c; }
    virtual TokenBase *clone(){ return new TokenSpace(cnt); }
    virtual TokenType type() const { return TokenType::ttSpace; }
    virtual TokenID   id()   const { return TokenID::tkSpace; }
};

// tab
class TokenTab: public TokenBase
{
public:
    int cnt;
    TokenTab() : TokenBase(9) { cnt = 0; }
    TokenTab(int c) : TokenBase(9) { cnt = c; }
    virtual TokenBase *clone(){ return new TokenTab(cnt); }
    virtual TokenType type() const { return TokenType::ttTab; }
    virtual TokenID   id()   const { return TokenID::tkTab; }
};

// end of line
class TokenEOL: public TokenBase
{
public:
    int cnt;
    TokenEOL() : TokenBase(13) { cnt = 0; }
    TokenEOL(int c) : TokenBase(13) { cnt = c; }
    virtual TokenBase *clone(){ return new TokenEOL(cnt); }
    virtual TokenType type() const { return TokenType::ttEOL; }
    virtual TokenID   id()   const { return TokenID::tkEOL; }
};

// operators

// single symbol operator base class
class TokenOperator: public TokenBase
{
protected:
    asmjit::x86::Xmm _xmm;
public:
    TokenBase *left;
    TokenBase *right;
    TokenOperator() : TokenBase() { left = NULL; right = NULL; _datatype = &ddINT; }
    TokenOperator(int t) : TokenBase(t) { left = NULL; right = NULL; _datatype = &ddINT; }
    virtual TokenBase *clone() { TokenOperator *to = new TokenOperator(); to->left = left; to->right = right; return to; }
    virtual int64_t ival() const { /*if (left && right) return operate();*/ return 0; }
    virtual size_t argc() const { return 2; }
    virtual bool is_operator() const override { return true; }
    virtual inline TokenType type()     const { return TokenType::ttOperator;     }
    virtual inline TokenID   id()       const { return TokenID::tkOperator;       }
    virtual inline TokenAssoc assoc()   const { return TokenAssoc::taLeftToRight; }
    virtual inline int precedence() const  { return 15; } // C Operator Precedence, default to 15 (lowest)
    virtual inline int64_t ioperate() const { return 0; } // integer operation
    virtual inline double foperate() const { return 0; } // floating point operation
    virtual void setregdp(Program &, regdefp_t &);
    virtual void settype(Program &, regdefp_t &);
    virtual asmjit::x86::Gp &getreg(Program &);
    virtual asmjit::Operand &operand(Program &);
    virtual bool can_optimize();
    virtual asmjit::Operand &optimize(Program &, regdefp_t &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp)
    {
	DBG(std::cout << "TokenOperator::compile() called on operator: " << _token << std::endl);
	throw "!!! TokenOperator::compile() !!!";
    }
    virtual inline bool operator>(const TokenOperator &op) // used to compare precedence
    {
	DBG(std::cout << "TokenOperator(" << (char)_token << ") comparing precedence(" << precedence() << ") > to TokenOperator(" << (char)op.get() << ") precedence(" << op.precedence() << ")" << std::endl);
	// if pecedence is the same, then associativity takes precedence
	// RightToLeft has "higher" precedence than LeftToRight
	if ( precedence() == op.precedence() )
	    return associativity() > op.associativity();
	return precedence() < op.precedence(); // lower number is "higher" precedence
    }
};

// multi-symbol operator base class
class TokenMultiOp: public TokenOperator
{
public:
    std::string str;
    TokenMultiOp() : TokenOperator() {}
    TokenMultiOp(const char *s)  : TokenOperator() { str = s; }
    TokenMultiOp(std::string &s) : TokenOperator() { str = s; }
    virtual TokenBase *clone() { TokenMultiOp *to = new TokenMultiOp(); to->left = left; to->right = right; return to; }
    virtual TokenType type() const { return TokenType::ttMultiOp; }
    virtual TokenID   id()   const { return TokenID::tkMultiOp; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp)  { throw "!!! TokenMultiOp::compile() !!!"; }
    virtual inline int precedence() const { return 16; }
};

// addition operator +
class TokenAdd: public TokenOperator
{
public:
    TokenAdd() : TokenOperator('+') {}
    virtual TokenBase *clone() { TokenAdd *to = new TokenAdd(); to->left = left; to->right = right; return to; }
    virtual inline int precedence() const { return 4; }
    virtual TokenID id() const { return TokenID::tkAdd; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual DataDef *datadef() const override
    {
	if ( left  && left->datadef()  && left->datadef()->is_pointer()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_pointer() ) return right->datadef();
	if ( left  && left->datadef()  && left->datadef()->is_complex()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_complex() ) return right->datadef();
	return TokenOperator::datadef();
    }
    inline int64_t ioperate() const { return left->ival() + right->ival(); }
    inline double foperate() const { return left->dval() + right->dval(); }
};

// top precedence operator
class TokenPrimary: public TokenOperator
{
public:
    TokenPrimary(int t) : TokenOperator(t) {}
    virtual TokenBase *clone() { TokenPrimary *to = new TokenPrimary(_token); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkOperator; }
    virtual inline int precedence() const { return 1; }
};

// substraction operator -
class TokenSub: public TokenOperator
{
public:
    TokenSub() : TokenOperator('-') {}
    virtual TokenBase *clone() { TokenSub *to = new TokenSub(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkSub; }
    virtual inline int precedence() const { return 4; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual DataDef *datadef() const override
    {
	// `p - n` is a pointer; `p - q` (both pointers) is ptrdiff_t.
	if ( left && left->datadef() && left->datadef()->is_pointer() )
	{
	    if ( right && right->datadef() && right->datadef()->is_pointer() )
		return TokenOperator::datadef();
	    return left->datadef();
	}
	if ( left  && left->datadef()  && left->datadef()->is_complex()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_complex() ) return right->datadef();
	return TokenOperator::datadef();
    }
    inline int64_t ioperate() const { return left->ival() - right->ival(); }
    inline double foperate() const { return left->dval() - right->dval(); }
};

// negative operator - (unary minus)
class TokenNeg: public TokenOperator
{
public:
    TokenNeg() : TokenOperator('-') {}
    virtual TokenBase *clone() { TokenNeg *to = new TokenNeg(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkNeg; }
    virtual inline int precedence() const { return 2; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
    inline int64_t ioperate() const {
	int64_t v = - right->ival();
	// Mask to operand width: -1U must yield 0xFFFFFFFF, not -1 as int64
	if ( right->datadef() && right->datadef()->is_unsigned() && right->datadef()->size < 8 )
	    v &= (int64_t)((1ULL << (right->datadef()->size * 8)) - 1);
	return v;
    }
    inline double foperate() const { return - right->dval(); }
    // Propagate unsigned operand type so -1U is uint32, not ddINT
    virtual DataDef *datadef() const override {
	if ( right && right->datadef() && right->datadef()->is_unsigned() )
	    return right->datadef();
	return TokenOperator::datadef();
    }
};

// multiply operator *
class TokenMul: public TokenOperator
{
public:
    TokenMul() : TokenOperator('*') {}
    virtual TokenBase *clone() { TokenMul *to = new TokenMul(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkMul; }
    virtual inline int precedence() const { return 3; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual DataDef *datadef() const override
    {
	if ( left  && left->datadef()  && left->datadef()->is_complex()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_complex() ) return right->datadef();
	return TokenOperator::datadef();
    }
    inline int64_t ioperate() const { return left->ival() * right->ival(); }
    inline double foperate() const { return left->dval() * right->dval(); }
};

// divide operator /
class TokenDiv: public TokenOperator
{
public:
    TokenDiv() : TokenOperator('/') {}
    virtual TokenBase *clone() { TokenDiv *to = new TokenDiv(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkDiv; }
    virtual inline int precedence() const { return 3; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual DataDef *datadef() const override
    {
	if ( left  && left->datadef()  && left->datadef()->is_complex()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_complex() ) return right->datadef();
	return TokenOperator::datadef();
    }
    inline int64_t ioperate() const { return left->ival() / right->ival(); }
    inline double foperate() const { return left->dval() / right->dval(); }
};

// modulo / remainder operator %
class TokenMod: public TokenOperator
{
public:
    TokenMod() : TokenOperator('%') {}
    virtual TokenBase *clone() { TokenMod *to = new TokenMod(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkMod; }
    virtual inline int precedence() const { return 3; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() % right->ival(); }
    inline double foperate() const { return ioperate(); }
};

// increment operator ++
class TokenInc: public TokenMultiOp
{
public:
    TokenInc() : TokenMultiOp("++") {}
    virtual TokenBase *clone() { TokenInc *to = new TokenInc(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkInc; }
    virtual DataDef *datadef() const override
    {
	if ( left )  return left->datadef();
	if ( right ) return right->datadef();
	return TokenBase::datadef();
    }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
    inline int64_t ioperate() const
    {
	if ( left )  { return left->ival() + 1;  }
	if ( right ) { return right->ival() + 1; }
	return 0;
    }
    inline double foperate() const
    {
	if ( left )  { return left->dval() + 1.0;  }
	if ( right ) { return right->dval() + 1.0; }
	return 0;
    }
};

// decrement operator --
class TokenDec: public TokenMultiOp
{
public:
    TokenDec() : TokenMultiOp("--") {}
    virtual TokenBase *clone() { TokenDec *to = new TokenDec(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkDec; }
    virtual DataDef *datadef() const override
    {
	if ( left )  return left->datadef();
	if ( right ) return right->datadef();
	return TokenBase::datadef();
    }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
    inline int64_t ioperate() const
    {
	if ( left )  { return left->ival() - 1;  }
	if ( right ) { return right->ival() - 1; }
	return 0;
    }
    inline double foperate() const
    {
	if ( left )  { return left->dval() - 1.0; }
	if ( right ) { return right->dval() - 1.0; }
	return 0;
    }
};

// assignment operator =
class TokenAssign: public TokenOperator
{
public:
    std::vector<Variable *> multi_vars; // for multi-return: a, b := func()
    TokenAssign() : TokenOperator('=') {}
    virtual TokenBase *clone() { TokenAssign *to = new TokenAssign(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkAssign; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual DataDef *datadef() const override
    {
	// An assignment-as-expression evaluates to the assigned LHS
	// value, so its type is the LHS's type — required for
	// `*(end = ptr + N)` where `end` is `char *`.
	if ( left && left->datadef() ) return left->datadef();
	return TokenOperator::datadef();
    }
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    int64_t ioperate() const;
};

// assignment operator += (assignment by sum)
class TokenAddEq: public TokenMultiOp
{
public:
    TokenAddEq() : TokenMultiOp("+=") {}
    virtual TokenID id() const { return TokenID::tkAddEq; }
    virtual TokenBase *clone() { return new TokenAddEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator -= (assignment by difference)
class TokenSubEq: public TokenMultiOp
{
public:
    TokenSubEq() : TokenMultiOp("-=") {}
    virtual TokenID id() const { return TokenID::tkSubEq; }
    virtual TokenBase *clone() { return new TokenSubEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator *= (assignment by product)
class TokenMulEq: public TokenMultiOp
{
public:
    TokenMulEq() : TokenMultiOp("*=") {}
    virtual TokenID id() const { return TokenID::tkMulEq; }
    virtual TokenBase *clone() { return new TokenMulEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator /= (assignment by quotient)
class TokenDivEq: public TokenMultiOp
{
public:
    TokenDivEq() : TokenMultiOp("/=") {}
    virtual TokenID id() const { return TokenID::tkDivEq; }
    virtual TokenBase *clone() { return new TokenDivEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator %= (assignment by remainder)
class TokenModEq: public TokenMultiOp
{
public:
    TokenModEq() : TokenMultiOp("%=") {}
    virtual TokenID id() const { return TokenID::tkModEq; }
    virtual TokenBase *clone() { return new TokenModEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator <<= (assignment by bitwise left shift)
class TokenBSLEq: public TokenMultiOp
{
public:
    TokenBSLEq() : TokenMultiOp("<<=") {}
    virtual TokenID id() const { return TokenID::tkBSLEq; }
    virtual TokenBase *clone() { return new TokenBSLEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator >>= (assignment by bitwise right shift)
class TokenBSREq: public TokenMultiOp
{
public:
    TokenBSREq() : TokenMultiOp(">>=") {}
    virtual TokenID id() const { return TokenID::tkBSREq; }
    virtual TokenBase *clone() { return new TokenBSREq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator &= (assignment by bitwise and)
class TokenBandEq: public TokenMultiOp
{
public:
    TokenBandEq() : TokenMultiOp("&=") {}
    virtual TokenID id() const { return TokenID::tkBandEq; }
    virtual TokenBase *clone() { return new TokenBandEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator |= (assignment by bitwise or)
class TokenBorEq: public TokenMultiOp
{
public:
    TokenBorEq() : TokenMultiOp("|=") {}
    virtual TokenID id() const { return TokenID::tkBorEq; }
    virtual TokenBase *clone() { return new TokenBorEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// assignment operator ^= (assignment by bitwise xor)
class TokenXorEq: public TokenMultiOp
{
public:
    TokenXorEq() : TokenMultiOp("^=") {}
    virtual TokenID id() const { return TokenID::tkXorEq; }
    virtual TokenBase *clone() { return new TokenXorEq(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 14; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
};

// overload function operator ()
class TokenFuncOp: public TokenMultiOp
{
public:
    TokenFuncOp() : TokenMultiOp("()") {}
    virtual TokenID id() const { return TokenID::tkFuncOp; }
    virtual TokenBase *clone() { return new TokenFuncOp(); }
    virtual inline int precedence() const { return 1; }
};

// overload array operator []
class TokenArrayOp: public TokenMultiOp
{
public:
    TokenArrayOp() : TokenMultiOp("[]") {}
    virtual TokenID id() const { return TokenID::tkArrayOp; }
    virtual TokenBase *clone() { return new TokenArrayOp(); }
    virtual inline int precedence() const { return 1; }
};

// bitwise not operator ~
class TokenBnot: public TokenOperator
{
public:
    TokenBnot() : TokenOperator('~') {}
    virtual TokenID id() const { return TokenID::tkBnot; }
    virtual TokenBase *clone() { return new TokenBnot(); }
    // Propagate operand type so ~0U is uint32, not the default ddINT.
    virtual DataDef *datadef() const override {
	if ( right && right->datadef() && right->datadef()->is_integer() && right->datadef() != &ddINT )
	    return right->datadef();
	return TokenOperator::datadef();
    }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
    inline int64_t ioperate() const {
	int64_t v = ~right->ival();
	// Mask to operand's semantic width: ~0U must yield 0xFFFFFFFF, not 0xFFFFFFFFFFFFFFFF
	if ( right->datadef() && right->datadef()->size < 8 )
	    v &= (int64_t)((1ULL << (right->datadef()->size * 8)) - 1);
	return v;
    }
    inline double foperate() const { return ioperate(); }
};

// logical not operator !
class TokenLnot: public TokenOperator
{
public:
    TokenLnot() : TokenOperator('!') {}
    virtual TokenID id() const { return TokenID::tkLnot; }
    virtual TokenBase *clone() { return new TokenLnot(); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
    inline int64_t ioperate() const { return !right->ival(); }
    inline double foperate() const { return !right->dval(); }
};

// bitwise and operator &
class TokenBand: public TokenOperator
{
public:
    TokenBand() : TokenOperator('&') {}
    virtual TokenID id() const { return TokenID::tkBand; }
    virtual TokenBase *clone() { return new TokenBand(); }
    virtual inline int precedence() const { return 8; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() & right->ival(); }
    inline double foperate() const { return ioperate(); }
};

// logical and operator &&
class TokenLand: public TokenMultiOp
{
public:
    TokenLand() : TokenMultiOp("&&") {}
    virtual TokenID id() const { return TokenID::tkLand; }
    virtual TokenBase *clone() { return new TokenLand(); }
    virtual inline int precedence() const { return 11; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() && right->ival(); }
    inline double foperate() const { return left->dval() && right->dval(); }
};

// bitwise or operator | (inclusive or)
class TokenBor: public TokenOperator
{
public:
    TokenBor() : TokenOperator('|') {}
    virtual TokenID id() const { return TokenID::tkBor; }
    virtual TokenBase *clone() { return new TokenBor(); }
    virtual inline int precedence() const { return 10; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() | right->ival(); }
    inline double foperate() const { return ioperate(); }
};

// logical or operator ||
class TokenLor: public TokenMultiOp
{
public:
    TokenLor() : TokenMultiOp("||") {}
    virtual TokenID id() const { return TokenID::tkLor; }
    virtual TokenBase *clone() { return new TokenLor(); }
    virtual inline int precedence() const { return 12; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() || right->ival(); }
    inline double foperate() const { return left->dval() || right->dval(); }
};

// bitwise xor operator ^ (exclusive or)
class TokenXor: public TokenOperator
{
public:
    TokenXor() : TokenOperator('^') {}
    virtual TokenID id() const { return TokenID::tkXor; }
    virtual TokenBase *clone() { return new TokenXor(); }
    virtual inline int precedence() const { return 9; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() ^ right->ival(); }
    inline double foperate() const { return ioperate(); }
};

// ternary operator ? (if)
class TokenTerQ: public TokenOperator
{
public:
    TokenBase *condition;
    TokenBase *true_expr;
    TokenBase *false_expr;
    TokenTerQ() : TokenOperator('?'), condition(NULL), true_expr(NULL), false_expr(NULL) {}
    virtual TokenID id() const { return TokenID::tkTerQ; }
    virtual TokenBase *clone() { return new TokenTerQ(); }
    virtual inline int precedence()   const { return 13; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() ? left->ival() : right->ival(); }
    inline double foperate() const { return left->dval() ? left->dval() : right->dval(); }
};

// ternary operator : (else)
class TokenTerC: public TokenOperator
{
public:
    TokenTerC() : TokenOperator(':') {}
    virtual TokenID id() const { return TokenID::tkTerC; }
    virtual TokenBase *clone() { return new TokenTerC(); }
    virtual inline int precedence()   const { return 13; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
};

// comparison operator == (equal to)
class TokenEquals: public TokenMultiOp
{
public:
    TokenEquals() : TokenMultiOp("==") {}
    virtual TokenID id() const { return TokenID::tkEquals; }
    virtual TokenBase *clone() { return new TokenEquals(); }
    virtual inline int precedence() const { return 7; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() == right->ival() ? 1 : 0; }
    inline double foperate() const { return left->dval() == right->dval() ? 1 : 0; }
};

// comparison operator === (exactly equal to)
class Token3Eq: public TokenMultiOp
{
public:
    Token3Eq() : TokenMultiOp("===") {}
    virtual TokenID id() const { return TokenID::tk3Eq; }
    virtual TokenBase *clone() { return new Token3Eq(); }
    virtual inline int precedence() const { return 7; }
    inline int64_t ioperate() const { return (left->datatype() == right->datatype() && left->ival() == right->ival()) ? 1 : 0; }
    inline double foperate() const { return (left->datatype() == right->datatype() && left->dval() == right->dval()) ? 1 : 0; }
};

// comparison operator != (not equal to)
class TokenNotEq: public TokenMultiOp
{
public:
    TokenNotEq() : TokenMultiOp("!=") {}
    virtual TokenID id() const { return TokenID::tkNotEq; }
    virtual TokenBase *clone() { return new TokenNotEq(); }
    virtual inline int precedence() const { return 7; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() != right->ival() ? 1 : 0; }
    inline double foperate() const { return left->dval() != right->dval() ? 1 : 0; }
};

// comparison operator < (less than)
class TokenLT: public TokenOperator
{
public:
    TokenLT() : TokenOperator('<') {}
    virtual TokenID id() const { return TokenID::tkLT; }
    virtual TokenBase *clone() { return new TokenLT(); }
    virtual inline int precedence() const { return 6; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() < right->ival() ? 1 : 0; }
    inline double foperate() const { return left->dval() < right->dval() ? 1 : 0; }
};

// comparison operator < (greater than)
class TokenGT: public TokenOperator
{
public:
    TokenGT() : TokenOperator('>') {}
    virtual TokenID id() const { return TokenID::tkGT; }
    virtual TokenBase *clone() { return new TokenGT(); }
    virtual inline int precedence() const { return 6; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() > right->ival() ? 1 : 0; }
    inline double foperate() const { return left->dval() > right->dval() ? 1 : 0; }
};

// comparison operator <= (less than or equal to)
class TokenLE: public TokenMultiOp
{
public:
    TokenLE() : TokenMultiOp("<=") {}
    virtual TokenID id() const { return TokenID::tkLE; }
    virtual TokenBase *clone() { return new TokenLE(); }
    virtual inline int precedence() const { return 6; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() <= right->ival() ? 1 : 0; }
    inline double foperate() const { return left->dval() <= right->dval() ? 1 : 0; }
};

// comparison operator <= (greater than or equal to)
class TokenGE: public TokenMultiOp
{
public:
    TokenGE() : TokenMultiOp(">=") {}
    virtual TokenID id() const { return TokenID::tkGE; }
    virtual TokenBase *clone() { return new TokenGE(); }
    virtual inline int precedence() const { return 6; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() >= right->ival() ? 1 : 0; }
    inline double foperate() const { return left->dval() >= right->dval() ? 1 : 0; }
};

// comparison operator <=> (three-way greater than, less than or equal to)
// evaluates to either -1 (<), 0 (=), or 1 (>)
class Token3Way: public TokenMultiOp
{
public:
    Token3Way() : TokenMultiOp("<=>") {}
    virtual TokenID id() const { return TokenID::tk3Way; }
    virtual TokenBase *clone() { return new Token3Way(); }
    virtual inline int precedence() const { return 6; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const 
    {
	if ( left->ival() < right->ival() ) { return -1; }
	if ( left->ival() > right->ival() ) { return 1;  }
	return 0;
    }
    inline double foperate() const { return ioperate(); }
};

// bitwise shift left <<
class TokenBSL: public TokenMultiOp
{
    public: TokenBSL() : TokenMultiOp("<<") {}
    virtual TokenID id() const { return TokenID::tkBSL; }
    virtual TokenBase *clone() { return new TokenBSL(); }
    virtual inline int precedence() const { return 5; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() << right->ival(); }
    inline double foperate() const { return ioperate(); }
};

// bitwise shift right >>
class TokenBSR: public TokenMultiOp
{
    public: TokenBSR() : TokenMultiOp(">>") {}
    virtual TokenID id() const { return TokenID::tkBSR; }
    virtual TokenBase *clone() { return new TokenBSR(); }
    virtual inline int precedence() const { return 5; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    inline int64_t ioperate() const { return left->ival() >> right->ival(); }
    inline double foperate() const { return ioperate(); }
};

// namespace operator ::
class TokenNS: public TokenMultiOp
{
public:
    TokenNS() : TokenMultiOp("::") {}
    virtual TokenID id() const { return TokenID::tkNS; }
    virtual TokenBase *clone() { return new TokenNS(); }
    virtual inline int precedence() const { return 1; }
};


// dereference struct/class operator ->
class TokenDeRef: public TokenMultiOp
{
public:
    TokenDeRef() : TokenMultiOp("->") {}
    virtual TokenID id() const { return TokenID::tkDeRef; }
    virtual TokenBase *clone() { return new TokenDeRef(); }
    virtual inline int precedence() const { return 1; }
};

// fat-arrow operator => — only meaningful inside a rust::match arm.
// Parsed by TokenMatch::parse(); the lexer just emits the token so the
// match parser can recognize arm boundaries without leaning on local
// `=` / `>` lookahead.
class TokenFatArrow: public TokenMultiOp
{
public:
    TokenFatArrow() : TokenMultiOp("=>") {}
    virtual TokenID id() const { return TokenID::tkFatArrow; }
    virtual TokenBase *clone() { return new TokenFatArrow(); }
};

// dot operator . (structure/union/class access)
class TokenDot: public TokenPrimary
{
public:
    TokenDot() : TokenPrimary('.') {}
    virtual TokenID id() const { return TokenID::tkDot; }
    virtual TokenBase *clone() { return new TokenDot(); }
    virtual inline int precedence() const { return 1; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// command operator , (perform first, second and return second result)
class TokenComma: public TokenOperator { public: TokenComma()  : TokenOperator(',') {} virtual TokenID id() const { return TokenID::tkComma; }  virtual TokenBase *clone() { return new TokenComma(); }
    // Comma operator: evaluate left for side effects, return right's value.
    // Used by parseExprStmt to chain `e1, e2, e3;` expression-statements
    // (notably brace-less while/for bodies like `++p, ++i;`). Without this,
    // parseExpression stopped at the first comma and the rest was dropped.
    virtual DataDef *datadef() const override {
	if ( right && right->datadef() )
	    return right->datadef();
	return TokenOperator::datadef();
    }
    virtual asmjit::Operand &compile(Program &, regdefp_t &);
};


// symbols

class TokenSymbol: public TokenBase
{
public:
    TokenSymbol() : TokenBase() {}
    TokenSymbol(int v) : TokenBase(v) {}
    virtual TokenBase *clone() { return new TokenSymbol(_token); }
    virtual TokenType type() const { return TokenType::ttSymbol; }
};

// symbol tokens
class TokenHash:  public TokenSymbol   { public: TokenHash()   :   TokenSymbol('#') {} virtual TokenID id() const { return TokenID::tkHash; }   virtual TokenBase *clone() { return new TokenHash(); } };
class TokenBslsh: public TokenSymbol   { public: TokenBslsh()  :  TokenSymbol('\\') {} virtual TokenID id() const { return TokenID::tkBslsh; }  virtual TokenBase *clone() { return new TokenBslsh(); } };
class TokenOpBrc: public TokenSymbol   { public: TokenOpBrc()  :   TokenSymbol('{') {} virtual TokenID id() const { return TokenID::tkOpBrc; }  virtual TokenBase *clone() { return new TokenOpBrc(); } };
class TokenClBrc: public TokenSymbol   { public: TokenClBrc()  :   TokenSymbol('}') {} virtual TokenID id() const { return TokenID::tkClBrc; }  virtual TokenBase *clone() { return new TokenClBrc(); } };
class TokenOpBrk: public TokenPrimary  { public: TokenOpBrk()  :  TokenPrimary('(') {} virtual TokenID id() const { return TokenID::tkOpBrk; }  virtual TokenBase *clone() { return new TokenOpBrk(); } };
class TokenClBrk: public TokenPrimary  { public: TokenClBrk()  :  TokenPrimary(')') {} virtual TokenID id() const { return TokenID::tkClBrk; }  virtual TokenBase *clone() { return new TokenClBrk(); } };
class TokenOpSqr: public TokenPrimary  { public: TokenOpSqr()  :  TokenPrimary('[') {} virtual TokenID id() const { return TokenID::tkOpSqr; }  virtual TokenBase *clone() { return new TokenOpSqr(); } };
class TokenClSqr: public TokenPrimary  { public: TokenClSqr()  :  TokenPrimary(']') {} virtual TokenID id() const { return TokenID::tkClSqr; }  virtual TokenBase *clone() { return new TokenClSqr(); } };
class TokenSemi:  public TokenSymbol   { public: TokenSemi()   :   TokenSymbol(';') {} virtual TokenID id() const { return TokenID::tkSemi; }   virtual TokenBase *clone() { return new TokenSemi(); } };
class TokenColEq: public TokenSymbol   { public: TokenColEq()  :  TokenSymbol(':') {} virtual TokenID id() const { return TokenID::tkColEq; } virtual TokenBase *clone() { return new TokenColEq(); } };
class TokenQuote: public TokenSymbol   { public: TokenQuote()  :   TokenSymbol('"') {} virtual TokenID id() const { return TokenID::tkQuote; }  virtual TokenBase *clone() { return new TokenQuote(); } };
class TokenApost: public TokenSymbol   { public: TokenApost()  :  TokenSymbol('\'') {} virtual TokenID id() const { return TokenID::tkApost; }  virtual TokenBase *clone() { return new TokenApost(); } };


// base numerics

class TokenChar: public TokenBase
{
public:
    TokenChar() : TokenBase()       { _datatype = &ddCHAR; }
    TokenChar(int v) : TokenBase(v) { _datatype = &ddCHAR; }
    virtual TokenBase *clone()      { return new TokenChar(_token); }
    virtual int64_t ival() const    { return _token; }
    virtual bool is_constant()	    { return true; }
    virtual TokenType type() const  { return TokenType::ttChar; }
    virtual TokenID   id()   const  { return TokenID::tkChar; }
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenInt: public TokenBase
{
public:
    std::string source_text;   // original literal text (hex, suffixes)
    TokenInt() : TokenBase()            { _datatype = &ddINT; }
    TokenInt(int64_t v) : TokenBase(v) { _datatype = &ddINT; }
    TokenInt(int64_t v, const std::string &src) : TokenBase(v), source_text(src) { _datatype = &ddINT; }
    virtual int64_t ival() const        { return _token; }
    virtual double dval() const    { return (double)_token; }
    virtual TokenType type() const { return TokenType::ttInteger; }
    virtual TokenID   id()   const { return TokenID::tkInt; }
    virtual TokenBase *clone()     { auto *c = new TokenInt(_token); c->source_text = source_text; c->_datatype = _datatype; return c; }
    virtual bool is_constant() const override { return true; }
    virtual void setDataType(DataDef *d) { if (d && (d->is_integer() || d->is_complex())) _datatype = d; }
//  virtual asmjit::x86::Gp &getreg(Program &);
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenNullptr: public TokenInt
{
public:
    TokenNullptr() : TokenInt(0) { _datatype = &ddVOIDptr; }
    virtual TokenBase *clone() { return new TokenNullptr(); }
    virtual void setDataType(DataDef *d)
    {
	if ( d && d->is_pointer() )
	    _datatype = d;
    }
};

class TokenTypeQuery: public TokenBase
{
public:
    DataDef *query_type;
    bool want_alignof;
    bool use_cached_runtime_size;

    TokenTypeQuery(DataDef *dd = NULL, bool want_align = false,
		   bool use_cached_size = true)
	: TokenBase(), query_type(dd), want_alignof(want_align),
	  use_cached_runtime_size(use_cached_size)
    {
	_datatype = &ddUINT64;
    }
    virtual TokenBase *clone() { return new TokenTypeQuery(query_type, want_alignof, use_cached_runtime_size); }
    virtual TokenID id() const { return TokenID::tkInt; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenReal: public TokenBase
{
protected:
    asmjit::x86::Mem _const;
    double _val;
public:
    TokenReal() : TokenBase()         { _val = 0; _datatype = &ddDOUBLE; }
    TokenReal(double v) : TokenBase() { _val = v; _datatype = &ddDOUBLE; }
    virtual int64_t ival() const      { return (int64_t)_val; }
    virtual double dval() const       { return _val;      }
    virtual TokenType type() const    { return TokenType::ttReal; }
    virtual TokenID   id()   const    { return TokenID::tkReal;   }
    virtual TokenBase *clone()        { return new TokenReal(_val); }
    virtual bool is_constant() const override { return true; }
    virtual bool is_real()     const override { return true; }
    virtual void setDataType(DataDef *d) { if (d && (d->is_real() || d->is_complex())) _datatype = d; }
//  virtual asmjit::x86::Gp &getreg(Program &) { throw "TokenReal::getreg(): Use TokenReal::operand()!"; }
    virtual asmjit::Operand &operand(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// string based tokens

// basic identifier
class TokenIdent: public TokenBase
{
public:
    std::string str;
    TokenIdent() { _datatype = &ddSTRING; }
    TokenIdent(std::string &s) { str = s; _datatype = &ddSTRING; }
    TokenIdent(const char *s)  { str = s; _datatype = &ddSTRING; }
    virtual TokenType type() const { return TokenType::ttIdentifier; }
    virtual TokenID   id()   const { return TokenID::tkIdent; }
    virtual TokenBase *clone()     { return new TokenIdent(str); }
    virtual void setDataType(DataDef *d) { if (d && d->is_string()) _datatype = d; }
};

// quoted string
class TokenStr: public TokenIdent
{
public:
    bool wide;
    TokenStr() : wide(false) {}
    TokenStr(const char *k, bool w = false) : TokenIdent(k), wide(w) {}
    TokenStr(std::string k, bool w = false) : TokenIdent(k), wide(w) {}
    virtual int64_t ival() const   { return atol(str.c_str()); }
    virtual bool is_constant() const override { return true; }
    virtual TokenType type() const { return TokenType::ttString; }
    virtual TokenID   id()   const { return TokenID::tkStr; }
    virtual TokenBase *clone()     { return new TokenStr(str, wide); }
};

// comment
class TokenREM: public TokenIdent
{
public:
    TokenREM() {}
    TokenREM(const char *k) : TokenIdent(k) {}
    TokenREM(std::string k) : TokenIdent(k) {}
    virtual bool is_constant() const override { return true; }
    virtual TokenType type() const { return TokenType::ttComment; }
    virtual TokenID   id()   const { return TokenID::tkREM; }
    virtual TokenBase *clone()     { return new TokenREM(str); }
};

// keyword tokens
class TokenKeyword: public TokenIdent
{
public:
    TokenKeyword(const char *k) : TokenIdent(k) {}
    TokenKeyword(std::string k) : TokenIdent(k) {}
    virtual TokenType type() const { return TokenType::ttKeyword; }
//  virtual TokenBase *clone(){ return new TokenKeyword(str); }
    virtual TokenBase *clone(){ return this; }
    virtual TokenBase *parse(Program &) { return NULL; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp)  { throw "!!! TokenKeyword::compile() !!!"; }
};

/*
32 C keywords needed:
auto         double      int        struct
break        else        long       switch
case         enum        register   typedef
char         extern      return     union
const        float       short      unsigned
continue     for         signed     void
default      goto        sizeof     volatile
do           if          static     while

31 additional C++ keywords
asm          bool        catch          class
const_cast   delete      dynamic_cast   explicit 
export       false       friend         inline 
mutable      namespace   new            operator 
private      protected   public         reinterpret_cast
static_cast  template    this           throw
true         try         typeid         typename 
using        virtual     wchar_t
*/

class TokenELSE:     public TokenKeyword { public: TokenELSE()     : TokenKeyword("else") {}     virtual TokenID id() const { return TokenID::tkELSE;     } virtual TokenBase *clone() { return (TokenBase*)new TokenELSE();    } };
// goto <label>; — unconditional jump to a named label in the
// enclosing function. Labels are function-scoped; forward references
// resolve through Program::label_map (populated on first goto or
// label reference, bound on the TokenLabel::compile pass).
class TokenGOTO: public TokenKeyword
{
public:
    std::string target;   // label name (set by parse)
    TokenBase *indirect_target;
    TokenGOTO() : TokenKeyword("goto"), indirect_target(NULL) {}
    virtual TokenID id() const { return TokenID::tkGOTO; }
    virtual TokenBase *clone() { return (TokenBase*)new TokenGOTO(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// `name:` — label statement (function-scoped). Binds the enclosing
// function's Program::label_map[name] at codegen time.
class TokenLabel: public TokenBase
{
public:
    std::string name;
    TokenLabel(const std::string &n) : name(n) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual TokenBase *clone() { return new TokenLabel(name); }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};
class TokenCASE: public TokenKeyword
{
public:
    TokenBase *value;                          // case constant expression
    TokenBase *range_high;                     // GNU case range: case LOW ... HIGH
    std::vector<TokenBase *> statements;       // statements until next case/default/}
    TokenCASE() : TokenKeyword("case"), value(NULL), range_high(NULL) {}
    virtual TokenID id() const { return TokenID::tkCASE; }
    virtual TokenBase *clone() { return new TokenCASE(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};
// try { ... } catch (type var) { ... } — exception handling
class TokenTRY: public TokenKeyword
{
public:
    TokenBase *try_body;                     // compound statement for try block
    // catch clauses: parallel vectors (type, var name, body)
    std::vector<int> catch_types;            // MADC_EXCEPT_* type tags (99 = catch(...))
    std::vector<std::string> catch_varnames; // catch variable names (empty for catch(...))
    std::vector<TokenBase *> catch_bodies;   // compound statements for each catch
    TokenTRY() : TokenKeyword("try") { try_body = NULL; }
    virtual TokenID id() const { return TokenID::tkTRY; }
    virtual TokenBase *clone() { return new TokenTRY(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};
class TokenCATCH:    public TokenKeyword { public: TokenCATCH()    : TokenKeyword("catch") {}    virtual TokenID id() const { return TokenID::tkCATCH;    } virtual TokenBase *clone() { return (TokenBase*)new TokenCATCH();   } };
// throw expr — throws an exception
class TokenTHROW: public TokenKeyword
{
public:
    TokenBase *throw_expr;  // expression to throw (NULL for rethrow)
    TokenTHROW() : TokenKeyword("throw") { throw_expr = NULL; }
    virtual TokenID id() const { return TokenID::tkTHROW; }
    virtual TokenBase *clone() { return new TokenTHROW(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};
class TokenSWITCH: public TokenKeyword
{
public:
    TokenBase *expression;                     // switch(expr)
    std::vector<TokenCASE *> cases;            // case entries
    TokenCASE *defaultcase;                    // default entry (reuses TokenCASE with value=NULL)
    int default_index;                         // source-order position of default among cases (-1 if none)
    TokenSWITCH() : TokenKeyword("switch"), expression(NULL), defaultcase(NULL), default_index(-1) {}
    virtual TokenID id() const { return TokenID::tkSWITCH; }
    virtual TokenBase *clone() { return new TokenSWITCH(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};
class TokenCLASS: public TokenKeyword
{
public:
    TokenCLASS() : TokenKeyword("class") {}
    virtual TokenID id() const { return TokenID::tkCLASS; }
    virtual TokenBase *clone() { return (TokenBase*)new TokenCLASS(); }
    virtual TokenBase *parse(Program &);
};
class TokenDEFAULT:  public TokenKeyword { public: TokenDEFAULT()  : TokenKeyword("default") {}  virtual TokenID id() const { return TokenID::tkDEFAULT;  } virtual TokenBase *clone() { return (TokenBase*)new TokenDEFAULT(); } };
class TokenTYPEDEF: public TokenKeyword
{
public:
    TokenTYPEDEF() : TokenKeyword("typedef") {}
    virtual TokenID id() const { return TokenID::tkTYPEDEF; }
    virtual TokenBase *clone() { return new TokenTYPEDEF(); }
    virtual TokenBase *parse(Program &pgm);
};

class TokenNAMESPACE:public TokenKeyword { public: TokenNAMESPACE() : TokenKeyword("namespace") {} virtual TokenID id() const { return TokenID::tkNAMESPACE; } virtual TokenBase *clone() { return (TokenBase*)new TokenNAMESPACE(); } };

class TokenUSING: public TokenKeyword
{
public:
    TokenUSING() : TokenKeyword("using") {}
    virtual TokenID id() const { return TokenID::tkUSING; }
    virtual TokenBase *clone() { return (TokenBase*)new TokenUSING(); }
    virtual TokenBase *parse(Program &);
};

class TokenPREFER: public TokenKeyword
{
public:
    TokenPREFER() : TokenKeyword("prefer") {}
    virtual TokenID id() const { return TokenID::tkPREFER; }
    virtual TokenBase *clone() { return (TokenBase*)new TokenPREFER(); }
    virtual TokenBase *parse(Program &);
};

// rust::match arm — one or more constant patterns and a single statement
// body.  is_wildcard distinguishes the `_` arm; patterns is empty in that
// case.  The body is a TokenBase* (could be a TokenCpnd from a `{ ... }`
// block, or any single statement).
class TokenMatch;
struct MatchArm
{
    std::vector<TokenBase *> patterns;  // TokenInt constants
    bool is_wildcard;
    TokenBase *body;
    MatchArm() : is_wildcard(false), body(NULL) {}
};

// TokenMatch — rust::match(expr) { p1 | p2 => stmt; _ => stmt; }
// Lowers to a compare-and-jump chain in compile().  v1 surface: integer
// patterns + `_` wildcard, no fall-through, multi-pattern arms with `|`.
class TokenMatch: public TokenKeyword
{
public:
    TokenBase *expression;          // scrutinee
    std::vector<MatchArm *> arms;   // source-order arms (including wildcard)
    int wildcard_index;             // source-order position of `_` arm, -1 if none
    TokenMatch() : TokenKeyword("match"), expression(NULL), wildcard_index(-1) {}
    virtual TokenID id() const { return TokenID::tkMATCH; }
    virtual TokenBase *clone() { return new TokenMatch(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

// defer keyword: register a statement to run at scope exit (LIFO)
class TokenDEFER: public TokenKeyword
{
public:
    TokenDEFER() : TokenKeyword("defer") {}
    virtual TokenID id() const { return TokenID::tkDEFER; }
    virtual TokenBase *clone() { return new TokenDEFER(); }
    virtual TokenBase *parse(Program &);
};

// STL container keywords — parse template syntax vector<type>, map<k,v>, etc.
class TokenVECTOR: public TokenKeyword { public: TokenVECTOR() : TokenKeyword("vector") {} virtual TokenID id() const { return TokenID::tkVECTOR; } virtual TokenBase *clone() { return new TokenVECTOR(); } virtual TokenBase *parse(Program &); };
class TokenMAP:    public TokenKeyword { public: TokenMAP()    : TokenKeyword("map") {}    virtual TokenID id() const { return TokenID::tkMAP; }    virtual TokenBase *clone() { return new TokenMAP(); }    virtual TokenBase *parse(Program &); };
class TokenSET:    public TokenKeyword { public: TokenSET()    : TokenKeyword("set") {}    virtual TokenID id() const { return TokenID::tkSET; }    virtual TokenBase *clone() { return new TokenSET(); }    virtual TokenBase *parse(Program &); };
class TokenLIST:   public TokenKeyword { public: TokenLIST()   : TokenKeyword("list") {}   virtual TokenID id() const { return TokenID::tkLIST; }   virtual TokenBase *clone() { return new TokenLIST(); }   virtual TokenBase *parse(Program &); };

// new / delete — heap allocation with constructor/destructor calls
class TokenNEW: public TokenKeyword
{
public:
    DataDefCLASS *alloc_class;
    std::vector<TokenBase *> ctor_args;
    TokenNEW() : TokenKeyword("new") { alloc_class = NULL; }
    virtual TokenID id() const { return TokenID::tkNEW; }
    virtual TokenBase *clone() { return new TokenNEW(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};
class TokenDELETE: public TokenKeyword
{
public:
    TokenBase *expr;
    DataDefCLASS *del_class;
    TokenDELETE() : TokenKeyword("delete") { expr = NULL; del_class = NULL; }
    virtual TokenID id() const { return TokenID::tkDELETE; }
    virtual TokenBase *clone() { return new TokenDELETE(); }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenSTRUCT: public TokenKeyword
{
public:
    TokenSTRUCT() : TokenKeyword("struct") {}
    virtual TokenID id() const { return TokenID::tkSTRUCT; }
    virtual TokenBase *clone() { return new TokenSTRUCT(); }
    virtual TokenBase *parse(Program &pgm);
};

class TokenUNION: public TokenSTRUCT
{
public:
    TokenUNION() : TokenSTRUCT() { str = "union"; }
    virtual TokenID id() const { return TokenID::tkUNION; }
    virtual TokenBase *clone() { return new TokenUNION(); }
};

// register keyword: declares a variable that lives only in a virtual register
// (never written to memory), for maximum performance in hot loops
class TokenREGISTER: public TokenKeyword
{
public:
    TokenREGISTER() : TokenKeyword("register") {}
    virtual TokenID id() const { return TokenID::tkREGISTER; }
    virtual TokenBase *clone() { return new TokenREGISTER(); }
    virtual TokenBase *parse(Program &pgm);
};

class TokenSTATIC: public TokenKeyword
{
public:
    TokenSTATIC() : TokenKeyword("static") {}
    virtual TokenID id() const { return TokenID::tkSTATIC; }
    virtual TokenBase *clone() { return new TokenSTATIC(); }
    virtual TokenBase *parse(Program &pgm);
};

// const and extern are consumed and ignored for C compatibility
class TokenCONST: public TokenKeyword
{
public:
    TokenCONST() : TokenKeyword("const") {}
    virtual TokenID id() const { return TokenID::tkCONST; }
    virtual TokenBase *clone() { return new TokenCONST(); }
    virtual TokenBase *parse(Program &pgm);
};

class TokenEXTERN: public TokenKeyword
{
public:
    TokenEXTERN() : TokenKeyword("extern") {}
    virtual TokenID id() const { return TokenID::tkEXTERN; }
    virtual TokenBase *clone() { return new TokenEXTERN(); }
    virtual TokenBase *parse(Program &pgm);
};

class TokenRESTRICT: public TokenKeyword
{
public:
    TokenRESTRICT() : TokenKeyword("restrict") {}
    virtual TokenID id() const { return TokenID::tkRESTRICT; }
    virtual TokenBase *clone() { return new TokenRESTRICT(); }
    virtual TokenBase *parse(Program &pgm);
};

class TokenVOLATILE: public TokenKeyword
{
public:
    TokenVOLATILE() : TokenKeyword("volatile") {}
    virtual TokenID id() const { return TokenID::tkVOLATILE; }
    virtual TokenBase *clone() { return new TokenVOLATILE(); }
    virtual TokenBase *parse(Program &pgm);
};

class TokenENUM: public TokenKeyword
{
public:
    TokenENUM() : TokenKeyword("enum") {}
    virtual TokenID id() const { return TokenID::tkENUM; }
    virtual TokenBase *clone() { return new TokenENUM(); }
    virtual TokenBase *parse(Program &pgm);
};

// va_arg(ap, type) — reads next variadic argument and advances va_list pointer
class TokenVaArg: public TokenBase
{
public:
    Variable *ap_var;      // the va_list variable (legacy, may be NULL)
    TokenBase *ap_expr;    // the va_list expression (may be subscript, deref, etc.)
    DataDef *target_type;  // the type to read as
    TokenVaArg(Variable *ap, DataDef *tt) : ap_var(ap), ap_expr(NULL), target_type(tt) { _datatype = tt; }
    TokenVaArg(Variable *ap, TokenBase *expr, DataDef *tt) : ap_var(ap), ap_expr(expr), target_type(tt) { _datatype = tt; }
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenBREAK: public TokenKeyword
{
public:
    TokenBREAK() : TokenKeyword("break") {}
    virtual TokenID id() const { return TokenID::tkBREAK; }
    virtual TokenBase *clone() { return new TokenBREAK(); }
    virtual TokenBase *parse(Program &pgm) { return this; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};

class TokenCONT: public TokenKeyword
{
public:
    TokenCONT() : TokenKeyword("continue") {}
    virtual TokenID id() const { return TokenID::tkCONT;  }
    virtual TokenBase *clone() { return new TokenCONT();  }
    virtual TokenBase *parse(Program &pgm) { return this; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
};


class TokenOPEROVER: public TokenKeyword
{
public:
    TokenOPEROVER() : TokenKeyword("operator") {}
    virtual TokenID id() const { return TokenID::tkOPEROVER; }
    virtual TokenBase *clone() { return new TokenOPEROVER(); }
    virtual TokenBase *parse(Program &);
};

class TokenIF: public TokenKeyword
{
public:
    TokenBase *condition;
    TokenBase *statement;
    TokenBase *elsestmt;
    TokenIF() : TokenKeyword("if") { condition = statement = elsestmt = NULL; }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual TokenID id() const { return TokenID::tkIF; }
    virtual TokenBase *clone() { return new TokenIF(); }
};

class TokenRETURN: public TokenKeyword
{
public:
    TokenBase *returns;
    std::vector<TokenBase *> return_exprs; // multi-return: return a, b;
    TokenRETURN() : TokenKeyword("return") { returns = NULL; }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual TokenID id() const { return TokenID::tkRETURN; }
    virtual TokenBase *clone() { return new TokenRETURN(); }
};

class TokenDO: public TokenKeyword
{
public:
    TokenBase *statement;
    TokenBase *condition;
    TokenDO() : TokenKeyword("do") { statement = condition = NULL; }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual TokenID id() const { return TokenID::tkDO; }
    virtual TokenBase *clone() { return new TokenDO(); }
};

class TokenWHILE: public TokenKeyword
{
public:
    TokenBase *condition;
    TokenBase *statement;
    TokenWHILE() : TokenKeyword("while") { condition = statement = NULL; }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual TokenID id() const { return TokenID::tkWHILE; }
    virtual TokenBase *clone() { return new TokenWHILE(); }
};

class TokenFOR: public TokenKeyword
{
public:
    TokenBase *initialize;
    TokenBase *condition;
    TokenBase *increment;
    TokenBase *statement;
    // C allows comma-separated expressions in the init and increment slots:
    //   for (a = 0, b = 1; cond; i++, j--)
    // The first one lives in `initialize` / `increment`; any extras here
    // run sequentially before/after at compile time.
    std::vector<TokenBase *> init_extras;
    std::vector<TokenBase *> incr_extras;
    TokenFOR() : TokenKeyword("for") { initialize = condition = increment = statement = NULL; }
    virtual TokenBase *parse(Program &);
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual TokenID id() const { return TokenID::tkFOR; }
    virtual TokenBase *clone() { return new TokenFOR(); }
};

// range-based for: for (type var : container) { ... }
class TokenFOREACH: public TokenKeyword
{
public:
    DataDef *elemtype;
    std::string elemname;
    Variable *elemvar;
    TokenBase *container;
    TokenBase *statement;
    TokenFOREACH() : TokenKeyword("for") { elemtype = NULL; elemvar = NULL; container = statement = NULL; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp);
    virtual TokenID id() const { return TokenID::tkFOR; }
    virtual TokenBase *clone() { return new TokenFOREACH(); }
};


#endif // __TOKENS_H
