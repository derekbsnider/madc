#ifndef __TOKENS_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Tokens								//
//									//
//////////////////////////////////////////////////////////////////////////
#define __TOKENS_H 1

// forward declaration
class Program;

enum class TokenType { 
//	0	1	 2	3	4	  5		6	  7
	ttBase, ttSpace, ttTab, ttEOL, ttComment, ttOperator, ttMultiOp, ttSymbol,
//	8		9	  10	 11	    12	    13		14	    15
	ttIdentifier, ttString, ttChar, ttInteger, ttReal, ttKeyword, ttBaseType, ttVariable,
//	16		17	  18		19	  20		21
	ttFunction, ttCallFunc, ttStatement, ttCompound, ttDeclare, ttProgram
};

enum class TokenID {
  tkBase, tkSpace, tkTab, tkEOL, tkREM, tkHash, tkAssign, tkEquals, tk3Eq, tkPlus, tkAdd=tkPlus, tkInc, tkSub,
  tkDash=tkSub, tkDec, tkMul, tkStar=tkMul, tkSlash, tkDiv=tkSlash, tkBslsh, tkOpBrc, tkClBrc, tkOpBrk, tkClBrk,
  tkOpSqr, tkClSqr, tkNot, tkBand, tkLand, tkBor, tkLor, tkXor, tkMod, tkQmark, tkTerQ=tkQmark, tkColon,
  tkTerC=tkColon, tkNS, tkSemi, tkComma, tkDot, tkDeRef, tkQuote, tkApost, tkGT, tkLT, tkBSL, tkBSR, tkAddEq,
  tkBSLEq, tkBSREq, tkBandEq, tkBnot, tkBorEq, tkDivEq, tkFuncOp, tkGE, tkLE, tkLnot, tkModEq, tkMulEq, tk3Way,
  tkNotEq, tkSubEq, tkXorEq, tkIdent, tkInt, tkChar, tkStr, tkOperator, tkDeclare, tkArrayOp, tkMultiOp,
// keywords
  tkDO, tkIF, tkFOR, tkELSE, tkRETURN, tkGOTO, tkCASE, tkBREAK, tkCONT, tkTRY, tkCATCH, tkTHROW,
  tkSWITCH, tkWHILE, tkCLASS, tkSTRUCT, tkDEFAULT, tkTYPEDEF, tkOPEROVER
};



class TokenBase
{
protected:
    int _token;
public:
    asmjit::x86::Gp _reg;
    const char *file;
    TokenBase *parent;
    int line;
    int column;
    std::streampos pos;
    TokenBase()      { _token = 0;    }
    TokenBase(int t) { _token = t;    }
    virtual TokenBase *clone() { return new TokenBase(_token); }
    virtual void set(int c)  { /*DBG(cout << "TokenBase::set(" << c << ')' << endl);*/ _token = c;    }
    virtual int inc() { return 0; }
    virtual int dec() { return 0; }
    virtual int get() const  { return _token; }
    virtual int val() const  { return 0; }
    virtual int argc() const { return 0; }
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual TokenID   id()   const { return TokenID::tkBase; }
    virtual asmjit::x86::Gp &getreg(Program &);
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
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
public:
    TokenBase *left;
    TokenBase *right;
    TokenOperator() : TokenBase() { left = NULL; right = NULL; }
    TokenOperator(int t) : TokenBase(t) { left = NULL; right = NULL; }
    virtual TokenBase *clone() { TokenOperator *to = new TokenOperator(); to->left = left; to->right = right; return to; }
    virtual int val() const { if (left && right) return operate(); return 0; }
    virtual int argc() const { return 2; }
    virtual TokenType type() const { return TokenType::ttOperator; }
    virtual TokenID   id()   const { return TokenID::tkOperator; }
    virtual inline int operate() const { return 0; } // used for internal debugging
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL)  { throw "!!! TokenOperator::compile() !!!"; }
    virtual inline int precedence() const { return 15; } // C Operator Precedence, default to 15 (lowest)
    virtual inline bool operator>=(const TokenOperator &op) // used to compare precedence
    {
	DBG(std::cout << "TokenOperator(" << (char)_token << ") comparing precedence(" << precedence() << ") >= to TokenOperator(" << (char)op.get() << ") precedence(" << op.precedence() << ")" << std::endl);
	if ( _token == op.get() )
	    return true;
	return precedence() <= op.precedence(); // lower is "higher"
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
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL)  { throw "!!! TokenMultiOp::compile() !!!"; }
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
    inline int operate() const
    {
	DBG(std::cout << "operate: " << left->get() << '+' << right->get() << std::endl);
	return left->val() + right->val();
    }
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
    inline int operate() const
    {
	DBG(std::cout << "operate: " << left->get() << '-' << right->get() << std::endl);
	return left->val() - right->val();
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
    inline int operate() const
    {
	DBG(std::cout << "operate: " << left->get() << '*' << right->get() << std::endl);
	return left->val() * right->val();
    }
};

// divide operator /
class TokenDiv: public TokenOperator
{
public:
    TokenDiv() : TokenOperator('/') {}
    virtual TokenBase *clone() { TokenDiv *to = new TokenDiv(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkDiv; }
    virtual inline int precedence() const { return 3; }
    inline int operate() const
    {
	DBG(std::cout << "operate: " << left->get() << '/' << right->get() << std::endl);
	return left->val() / right->val();
    }
};

// modulo / remainder operator %
class TokenMod: public TokenOperator
{
public:
    TokenMod() : TokenOperator('%') {}
    virtual TokenBase *clone() { TokenMod *to = new TokenMod(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkMod; }
    virtual inline int precedence() const { return 3; }
    inline int operate() const
    {
	DBG(std::cout << "operate: " << left->get() << '%' << right->get() << std::endl);
	return left->val() % right->val();
    }
};

// increment operator ++
class TokenInc: public TokenMultiOp
{
public:
    TokenInc() : TokenMultiOp("++") {}
    virtual TokenBase *clone() { TokenInc *to = new TokenInc(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkInc; }
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
    virtual inline int precedence() const { return 2; }
    virtual int argc() const { return 1; }
    inline int operate() const
    {
	if ( left )  { return left->inc(); }
	if ( right ) { return right->inc(); }
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
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
    virtual inline int precedence() const { return 2; }
    virtual int argc() const { return 1; }
    inline int operate() const
    {
	if ( left )  { return left->dec(); }
	if ( right ) { return right->dec(); }
	return 0;
    }
};

// assignment operator =
class TokenAssign: public TokenOperator
{
public:
    TokenAssign() : TokenOperator('=') {}
    virtual TokenBase *clone() { TokenAssign *to = new TokenAssign(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkAssign; }
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
    virtual inline int precedence() const { return 14; }
    int operate() const;
};

// assignment operator += (assignment by sum)
class TokenAddEq: public TokenMultiOp
{
public:
    TokenAddEq() : TokenMultiOp("+=") {}
    virtual TokenID id() const { return TokenID::tkAddEq; }
    virtual TokenBase *clone() { return new TokenAddEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator -= (assignment by difference)
class TokenSubEq: public TokenMultiOp
{
public:
    TokenSubEq() : TokenMultiOp("-=") {}
    virtual TokenID id() const { return TokenID::tkSubEq; }
    virtual TokenBase *clone() { return new TokenSubEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator *= (assignment by product)
class TokenMulEq: public TokenMultiOp
{
public:
    TokenMulEq() : TokenMultiOp("*=") {}
    virtual TokenID id() const { return TokenID::tkMulEq; }
    virtual TokenBase *clone() { return new TokenMulEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator /= (assignment by quotient)
class TokenDivEq: public TokenMultiOp
{
public:
    TokenDivEq() : TokenMultiOp("/=") {}
    virtual TokenID id() const { return TokenID::tkDivEq; }
    virtual TokenBase *clone() { return new TokenDivEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator %= (assignment by remainder)
class TokenModEq: public TokenMultiOp
{
public:
    TokenModEq() : TokenMultiOp("%=") {}
    virtual TokenID id() const { return TokenID::tkModEq; }
    virtual TokenBase *clone() { return new TokenModEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator <<= (assignment by bitwise left shift)
class TokenBSLEq: public TokenMultiOp
{
public:
    TokenBSLEq() : TokenMultiOp("<<=") {}
    virtual TokenID id() const { return TokenID::tkBSLEq; }
    virtual TokenBase *clone() { return new TokenBSLEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator >>= (assignment by bitwise right shift)
class TokenBSREq: public TokenMultiOp
{
public:
    TokenBSREq() : TokenMultiOp(">>=") {}
    virtual TokenID id() const { return TokenID::tkBSREq; }
    virtual TokenBase *clone() { return new TokenBSREq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator &= (assignment by bitwise and)
class TokenBandEq: public TokenMultiOp
{
public:
    TokenBandEq() : TokenMultiOp("&=") {}
    virtual TokenID id() const { return TokenID::tkBandEq; }
    virtual TokenBase *clone() { return new TokenBandEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator |= (assignment by bitwise or)
class TokenBorEq: public TokenMultiOp
{
public:
    TokenBorEq() : TokenMultiOp("|=") {}
    virtual TokenID id() const { return TokenID::tkBorEq; }
    virtual TokenBase *clone() { return new TokenBorEq(); }
    virtual inline int precedence() const { return 14; }
};

// assignment operator ^= (assignment by bitwise xor)
class TokenXorEq: public TokenMultiOp
{
public:
    TokenXorEq() : TokenMultiOp("^=") {}
    virtual TokenID id() const { return TokenID::tkXorEq; }
    virtual TokenBase *clone() { return new TokenXorEq(); }
    virtual inline int precedence() const { return 14; }
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
    virtual inline int precedence() const { return 2; }
    virtual int argc() const { return 1; }
};

// logical not operator !
class TokenLnot: public TokenOperator
{
public:
    TokenLnot() : TokenOperator('!') {}
    virtual TokenID id() const { return TokenID::tkLnot; }
    virtual TokenBase *clone() { return new TokenLnot(); }
    virtual inline int precedence() const { return 2; }
    virtual int argc() const { return 1; }
};

// bitwise and operator &
class TokenBand: public TokenOperator
{
public:
    TokenBand() : TokenOperator('&') {}
    virtual TokenID id() const { return TokenID::tkBand; }
    virtual TokenBase *clone() { return new TokenBand(); }
    virtual inline int precedence() const { return 8; }
};

// logical and operator &&
class TokenLand: public TokenMultiOp
{
public:
    TokenLand() : TokenMultiOp("&&") {}
    virtual TokenID id() const { return TokenID::tkLand; }
    virtual TokenBase *clone() { return new TokenLand(); }
    virtual inline int precedence() const { return 11; }
};

// bitwise or operator | (inclusive or)
class TokenBor: public TokenOperator
{
public:
    TokenBor() : TokenOperator('|') {}
    virtual TokenID id() const { return TokenID::tkBor; }
    virtual TokenBase *clone() { return new TokenBor(); }
    virtual inline int precedence() const { return 10; }
};

// logical or operator ||
class TokenLor: public TokenMultiOp
{
public:
    TokenLor() : TokenMultiOp("||") {}
    virtual TokenID id() const { return TokenID::tkLor; }
    virtual TokenBase *clone() { return new TokenLor(); }
    virtual inline int precedence() const { return 12; }
};

// bitwise xor operator ^ (exclusive or)
class TokenXor: public TokenOperator
{
public:
    TokenXor() : TokenOperator('^') {}
    virtual TokenID id() const { return TokenID::tkXor; }
    virtual TokenBase *clone() { return new TokenXor(); }
    virtual inline int precedence() const { return 9; }
};

// ternary operator ? (if)
class TokenTerQ: public TokenOperator
{
public:
    TokenTerQ() : TokenOperator('?') {}
    virtual TokenID id() const { return TokenID::tkTerQ; }
    virtual TokenBase *clone() { return new TokenTerQ(); }
    virtual inline int precedence() const { return 13; }
    virtual int argc() const { return 1; }
};

// ternary operator : (else)
class TokenTerC: public TokenOperator
{
public:
    TokenTerC() : TokenOperator(':') {}
    virtual TokenID id() const { return TokenID::tkTerC; }
    virtual TokenBase *clone() { return new TokenTerC(); }
    virtual inline int precedence() const { return 13; }
    virtual int argc() const { return 1; }
};

// comparison operator == (equal to)
class TokenEquals: public TokenMultiOp
{
public:
    TokenEquals() : TokenMultiOp("==") {}
    virtual TokenID id() const { return TokenID::tkEquals; }
    virtual TokenBase *clone() { return new TokenEquals(); }
    virtual inline int precedence() const { return 7; }
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
};

// comparison operator === (exactly equal to)
class Token3Eq: public TokenMultiOp
{
public:
    Token3Eq() : TokenMultiOp("===") {}
    virtual TokenID id() const { return TokenID::tk3Eq; }
    virtual TokenBase *clone() { return new Token3Eq(); }
    virtual inline int precedence() const { return 7; }
};

// comparison operator != (not equal to)
class TokenNotEq: public TokenMultiOp
{
public:
    TokenNotEq() : TokenMultiOp("!=") {}
    virtual TokenID id() const { return TokenID::tkNotEq; }
    virtual TokenBase *clone() { return new TokenNotEq(); }
    virtual inline int precedence() const { return 7; }
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
};

// comparison operator < (less than)
class TokenLT: public TokenOperator
{
public:
    TokenLT() : TokenOperator('<') {}
    virtual TokenID id() const { return TokenID::tkLT; }
    virtual TokenBase *clone() { return new TokenLT(); }
    virtual inline int precedence() const { return 6; }
};

// comparison operator < (greater than)
class TokenGT: public TokenOperator
{
public:
    TokenGT() : TokenOperator('>') {}
    virtual TokenID id() const { return TokenID::tkGT; }
    virtual TokenBase *clone() { return new TokenGT(); }
    virtual inline int precedence() const { return 6; }
};

// comparison operator <= (less than or equal to)
class TokenLE: public TokenMultiOp
{
public:
    TokenLE() : TokenMultiOp("<=") {}
    virtual TokenID id() const { return TokenID::tkLE; }
    virtual TokenBase *clone() { return new TokenLE(); }
    virtual inline int precedence() const { return 6; }
};

// comparison operator <= (greater than or equal to)
class TokenGE: public TokenMultiOp
{
public:
    TokenGE() : TokenMultiOp(">=") {}
    virtual TokenID id() const { return TokenID::tkGE; }
    virtual TokenBase *clone() { return new TokenGE(); }
    virtual inline int precedence() const { return 6; }
};

// comparison operator <=> (three-way greater than, less than or equal to)
class Token3Way: public TokenMultiOp
{
public:
    Token3Way() : TokenMultiOp("<=>") {}
    virtual TokenID id() const { return TokenID::tk3Way; }
    virtual TokenBase *clone() { return new Token3Way(); }
    virtual inline int precedence() const { return 6; }
};

// bitwise shift left <<
class TokenBSL: public TokenMultiOp
{
    public: TokenBSL() : TokenMultiOp("<<") {}
    virtual TokenID id() const { return TokenID::tkBSL; }
    virtual TokenBase *clone() { return new TokenBSL(); }
    virtual inline int precedence() const { return 5; }
};

// bitwise shift right >>
class TokenBSR: public TokenMultiOp
{
    public: TokenBSR() : TokenMultiOp(">>") {}
    virtual TokenID id() const { return TokenID::tkBSR; }
    virtual TokenBase *clone() { return new TokenBSR(); }
    virtual inline int precedence() const { return 5; }
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

// dot operator . (structure/union/class access)
class TokenDot:   public TokenPrimary  { public: TokenDot()    :  TokenPrimary('.') {} virtual TokenID id() const { return TokenID::tkDot; }    virtual TokenBase *clone() { return new TokenDot(); } };

// command operator , (perform first, second and return second result)
class TokenComma: public TokenOperator { public: TokenComma()  : TokenOperator(',') {} virtual TokenID id() const { return TokenID::tkComma; }  virtual TokenBase *clone() { return new TokenComma(); } };


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
class TokenQuote: public TokenSymbol   { public: TokenQuote()  :   TokenSymbol('"') {} virtual TokenID id() const { return TokenID::tkQuote; }  virtual TokenBase *clone() { return new TokenQuote(); } };
class TokenApost: public TokenSymbol   { public: TokenApost()  :  TokenSymbol('\'') {} virtual TokenID id() const { return TokenID::tkApost; }  virtual TokenBase *clone() { return new TokenApost(); } };


// base numerics

class TokenChar: public TokenBase
{
public:
    TokenChar() : TokenBase() {}
    TokenChar(int v) : TokenBase(v) {}
    virtual TokenBase *clone(){ return new TokenChar(_token); }
    virtual int val() const  { return _token; }
    virtual TokenType type() const { return TokenType::ttChar; }
    virtual TokenID   id()   const { return TokenID::tkChar; }
};

class TokenInt: public TokenBase
{
public:
    TokenInt() : TokenBase() {}
    TokenInt(int v) : TokenBase(v) {}
    virtual int val() const  { return _token; }
    virtual TokenType type() const { return TokenType::ttInteger; }
    virtual TokenID   id()   const { return TokenID::tkInt; }
    virtual TokenBase *clone()     { return new TokenInt(_token); }
    virtual asmjit::x86::Gp &getreg(Program &);
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
};

// string based tokens

// basic identifier
class TokenIdent: public TokenBase
{
public:
    std::string str;
    TokenIdent() {}
    TokenIdent(std::string &s) { str = s; }
    TokenIdent(const char *s)  { str = s; }
    virtual TokenType type() const { return TokenType::ttIdentifier; }
    virtual TokenID   id()   const { return TokenID::tkIdent; }
    virtual TokenBase *clone()     { return new TokenIdent(str); }
};

// quoted string
class TokenStr: public TokenIdent
{
public:
    TokenStr() {}
    TokenStr(const char *k) : TokenIdent(k) {}
    TokenStr(std::string k) : TokenIdent(k) {}
    virtual int val() const  { return atol(str.c_str()); }
    virtual TokenType type() const { return TokenType::ttString; }
    virtual TokenID   id()   const { return TokenID::tkStr; }
    virtual TokenBase *clone()     { return new TokenStr(str); }
};

// comment
class TokenREM: public TokenIdent
{
public:
    TokenREM() {}
    TokenREM(const char *k) : TokenIdent(k) {}
    TokenREM(std::string k) : TokenIdent(k) {}
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
    virtual TokenKeyword *parse(Program &) { return NULL; }
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL)  { throw "!!! TokenKeyword::compile() !!!"; }
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

class TokenELSE:     public TokenKeyword { public: TokenELSE()     : TokenKeyword("else") {}     virtual TokenID id() const { return TokenID::tkELSE;     } virtual TokenBase *clone() { return new TokenELSE();    } };
class TokenGOTO:     public TokenKeyword { public: TokenGOTO()     : TokenKeyword("goto") {}     virtual TokenID id() const { return TokenID::tkGOTO;     } virtual TokenBase *clone() { return new TokenGOTO();    } };
class TokenCASE:     public TokenKeyword { public: TokenCASE()     : TokenKeyword("case") {}     virtual TokenID id() const { return TokenID::tkCASE;     } virtual TokenBase *clone() { return new TokenCASE();    } };
class TokenBREAK:    public TokenKeyword { public: TokenBREAK()    : TokenKeyword("break") {}    virtual TokenID id() const { return TokenID::tkBREAK;    } virtual TokenBase *clone() { return new TokenBREAK();   } };
class TokenCONT:     public TokenKeyword { public: TokenCONT()     : TokenKeyword("continue") {} virtual TokenID id() const { return TokenID::tkCONT;     } virtual TokenBase *clone() { return new TokenCONT();    } };
class TokenTRY:      public TokenKeyword { public: TokenTRY()      : TokenKeyword("try") {}      virtual TokenID id() const { return TokenID::tkTRY;      } virtual TokenBase *clone() { return new TokenTRY();     } };
class TokenCATCH:    public TokenKeyword { public: TokenCATCH()    : TokenKeyword("catch") {}    virtual TokenID id() const { return TokenID::tkCATCH;    } virtual TokenBase *clone() { return new TokenCATCH();   } };
class TokenTHROW:    public TokenKeyword { public: TokenTHROW()    : TokenKeyword("throw") {}    virtual TokenID id() const { return TokenID::tkTHROW;    } virtual TokenBase *clone() { return new TokenTHROW();   } };
class TokenSWITCH:   public TokenKeyword { public: TokenSWITCH()   : TokenKeyword("switch") {}   virtual TokenID id() const { return TokenID::tkSWITCH;   } virtual TokenBase *clone() { return new TokenSWITCH();  } };
class TokenCLASS:    public TokenKeyword { public: TokenCLASS()    : TokenKeyword("class") {}    virtual TokenID id() const { return TokenID::tkCLASS;    } virtual TokenBase *clone() { return new TokenCLASS();   } };
class TokenSTRUCT:   public TokenKeyword { public: TokenSTRUCT()   : TokenKeyword("struct") {}   virtual TokenID id() const { return TokenID::tkSTRUCT;   } virtual TokenBase *clone() { return new TokenSTRUCT();  } };
class TokenDEFAULT:  public TokenKeyword { public: TokenDEFAULT()  : TokenKeyword("default") {}  virtual TokenID id() const { return TokenID::tkDEFAULT;  } virtual TokenBase *clone() { return new TokenDEFAULT(); } };
class TokenTYPEDEF:  public TokenKeyword { public: TokenTYPEDEF()  : TokenKeyword("typedef") {}  virtual TokenID id() const { return TokenID::tkTYPEDEF;  } virtual TokenBase *clone() { return new TokenTYPEDEF(); } };

class TokenOPEROVER: public TokenKeyword
{
public:
    TokenOPEROVER() : TokenKeyword("operator") {}
    virtual TokenID id() const { return TokenID::tkOPEROVER; }
    virtual TokenBase *clone() { return new TokenOPEROVER(); }
    TokenOPEROVER *parse(Program &);
};

class TokenIF: public TokenKeyword
{
public:
    TokenBase *condition;
    TokenBase *statement;
    TokenBase *elsestmt;
    TokenIF() : TokenKeyword("if") { condition = statement = elsestmt = NULL; }
    TokenIF *parse(Program &);
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
    virtual TokenID id() const { return TokenID::tkIF; }
    virtual TokenBase *clone() { return new TokenIF(); }
};

class TokenRETURN: public TokenKeyword
{
public:
    TokenBase *returns;
    TokenRETURN() : TokenKeyword("return") { returns = NULL; }
    TokenRETURN *parse(Program &);
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
    virtual TokenID id() const { return TokenID::tkRETURN; }
    virtual TokenBase *clone() { return new TokenRETURN(); }
};

class TokenDO: public TokenKeyword
{
public:
    TokenBase *statement;
    TokenBase *condition;
    TokenDO() : TokenKeyword("do") { statement = condition = NULL; }
    TokenDO *parse(Program &);
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
    virtual TokenID id() const { return TokenID::tkDO; }
    virtual TokenBase *clone() { return new TokenDO(); }
};

class TokenWHILE: public TokenKeyword
{
public:
    TokenBase *condition;
    TokenBase *statement;
    TokenWHILE() : TokenKeyword("while") { condition = statement = NULL; }
    TokenWHILE *parse(Program &);
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
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
    TokenFOR() : TokenKeyword("for") { initialize = condition = increment = statement = NULL; }
    TokenFOR *parse(Program &);
    virtual void compile(Program &, asmjit::x86::Gp *ret=NULL, asmjit::Label *l_true=NULL, asmjit::Label *l_false=NULL);
    virtual TokenID id() const { return TokenID::tkFOR; }
    virtual TokenBase *clone() { return new TokenFOR(); }
};


#endif // __TOKENS_H