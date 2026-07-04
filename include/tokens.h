#ifndef __TOKENS_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Tokens								//
//									//
//////////////////////////////////////////////////////////////////////////
#define __TOKENS_H 1

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <ios>
#include "madcdis/intern_table.h"
#include "madcdis/value_pool.h"

// forward declaration
class Program;
//class DataDef;

enum class TokenType {
//	0	1	 2	3	4	  5		6	  7
	ttBase, ttSpace, ttTab, ttEOL, ttComment, ttOperator, ttMultiOp, ttSymbol,
//	8		9	  10	 11	    12	    13		14	    15
	ttIdentifier, ttString, ttChar, ttInteger, ttReal, ttKeyword, ttDataType, ttVariable,
//	16		17	  18		19	  20		21	22	 23
	ttFunction, ttCallFunc, ttStatement, ttCompound, ttDeclare, ttProgram, ttMember, ttCallMethod, ttSubscript,
	ttStructLit,
	ttTypedefDecl,	// typedef declaration (preserves source order in AST)
	ttStructDef	// standalone struct/union definition (preserves source order in AST)
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
  tkTEMPLATE,
  tkFatArrow, tkMATCH,    // => (rust::match arm) and the match statement itself
  tkUNION, tkNEW, tkDELETE,
  tkDynamicCast, tkTypeid,    // RTTI (S5): dynamic_cast<T*>(e), typeid(e|T)
  tkObjTemp,                  // functional construction temporary: T(args)
  tk3NotEq,                   // !== strict not-equal (STD_MADC dialect)
  tkFRIEND,                   // C++ `friend` declaration specifier
  tkExplicitDtor,             // explicit/pseudo destructor call: obj.~T() / ptr->~T()
  tkCPPKEYWORD                // generic reserved C++ keyword (version-gated); the
			      // spelling distinguishes it. Used for reserved words
			      // that the parser still recognizes by spelling (via
			      // contextual_identifier_name) rather than a dedicated
			      // dispatch token — so they are reserved (not bare
			      // identifiers) without proliferating one class each.
};

enum class TokenAssoc {
    taNone, taLeftToRight, taRightToLeft
};


// Token flags
typedef enum : uint16_t { tfBRACKETED	=    1,
			  tfOVERLOADED  =    2,
			} tokflag_t;

// TokenRec — the flat, POD, serializable per-token DATA record (Phase 2 of
// docs/plans/2026-06-23-p1-token-arena-implementation-plan.md). Held by
// COMPOSITION on TokenBase (the diamond virtual inheritance — TokenVar/TokenCpnd
// vbase — rules out a base-class TokenRec; Phase 0.3). Fields migrate here one
// per commit; the end state is that ALL of a token's serializable data lives in
// `rec`, so the live tree can be dumped/mmapped and the polymorphic shell rebuilt
// by `kind` (the ROM the mutable pop-2 parse node references by slot-id).
// Populated for every lexed pop-1 token by Program::finalize_pop1_rec (step 1 of
// the no-clone split): kind, value, spelling_id, line/column, file_id. `type_id`
// is resolved lazily at materialize time (type_id_for assigns), NOT stamped here.
// pop-1 has NO children — it is a linear stream; trees are pop-2 (mutable).
struct TokenRec {				// trivially copyable (scalars only)
    uint16_t kind = 0;		// TokenID — the identity; drives shell rebuild
    uint16_t flags = 0;		// tokflag_t (reserved — immutable lex flags)
    uint32_t spelling_id = 0;	// -> Program::strpool (0 = none / not interned):
				// identifier/keyword name, string/comment bytes,
				// or datatype spelling — the string payload.
    uint32_t slot_id = 0;	// -> TokenArena slot registry (0 = unassigned); the
				// token's stable identity for child / serialization
				// links (replaces raw TokenBase* in id-vectors).
    uint32_t type_id = 0;	// -> type table (reserved; resolved at materialize
				// time, NOT here — type_id_for() assigns lazily).
    uint32_t file_id = 0;	// -> Program::strpool (interned filename); provenance
    int32_t  line = 0;		// provenance — PER record (the lex occurrence)
    int32_t  column = 0;
    int64_t  value = 0;		// _token / char code / double-bits (by kind);
				// >64-bit literals -> value-pool handle (P0, later).
};

class TokenBase
{
protected:
    int64_t _token;
    DataDef *_datatype;
    uint16_t _flags;
public:
    const char *file;
    TokenBase *parent;
    int line;
    int column;
    std::streampos pos;
    // Flat POD data record (Phase 2). See TokenRec above.
    TokenRec rec;
    // Diagnostic: how many times the parser has CONSUMED this token via
    // nextToken() (a re-read > 1 means backtracking / pushback re-lexing /
    // template re-instantiation touched the same token object). Reported in
    // aggregate by --show-stats; otherwise just one uint per token.
    uint32_t read_count;
    // Leading trivia (whitespace + comments) preserved before this token, for
    // byte-faithful source reconstruction. Populated only in full-fidelity mode
    // (Program::keep_trivia); empty in lean/batch mode (zero cost there).
    std::string leading_trivia;
    // Current parse position — updated by nextToken(), inherited by
    // all new tokens so synthetic parser-created tokens automatically
    // get the position of the most recently consumed source token.
    static const char *_parse_file;
    static int _parse_line;
    static int _parse_column;
    // Active interned-spelling pool for spelling() (interning Step 4). Bound to the
    // currently-processing Program's strpool at lex/parse entry (compile is
    // sequential per-Program, incl. --project per-TU). Lets the arg-less spelling()
    // accessor resolve rec.spelling_id -> bytes without a Program* on every token.
    static madc::dis::intern_table *_active_strpool;
    // The active Program's value pool (P0 slice 2/3): wide (>64-bit) constant
    // payloads live here under TokenInt::wide_handle. Same active-owner
    // discipline as _active_strpool above.
    static madc::dis::value_pool *_active_valpool;
    TokenBase()           { _token = 0; _datatype = &ddVOID; _flags = 0; file = _parse_file; parent = NULL; line = _parse_line; column = _parse_column; pos = 0; read_count = 0; }
    TokenBase(int64_t t)  { _token = t; _datatype = &ddVOID; _flags = 0; file = _parse_file; parent = NULL; line = _parse_line; column = _parse_column; pos = 0; read_count = 0; }
    virtual ~TokenBase() {}
    // Every token (and every clone()) allocates from the per-process TokenArena
    // (token_arena.h). operator delete is a no-op: tokens are never individually
    // freed — a `delete tok` still runs the virtual destructor (freeing that
    // token's std::string members) but the arena cell is reclaimed only when the
    // arena is dropped. See docs/plans/2026-06-23-p1-token-arena-implementation-plan.md.
    static void *operator new(std::size_t sz);
    static void  operator delete(void *p);
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
    // The 128-bit constant view (P0 slice 3). Returns the full wide value for
    // a token that is SEMANTICALLY 128-bit-typed (a parser-folded wide
    // constant); every other token answers its ival(). A lexer-truncated
    // too-large literal keeps its truncated value here too — gcc canon: the
    // literal's value IS the truncated one (the retained payload under
    // TokenInt::wide_handle is diagnostics-only in that case).
    virtual madc_wide_int wival() const { return ival(); }
    virtual double dval() const { return 0; }
    virtual size_t argc() const { return 0; }
    virtual TokenType  type()  const { return TokenType::ttBase; }
    virtual TokenID    id()    const { return TokenID::tkBase; }
    virtual DataType datatype() const { return _datatype ? _datatype->type() : DataType::dtVOID; }
    virtual DataDef *datadef()  const { return _datatype ? _datatype : &ddVOID; }
    virtual TokenAssoc associativity() const { return TokenAssoc::taNone; }
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
    // When this operator is an OVERLOADED operator on a class object, the result
    // type is the operator method's return type — set by the parser at reduce
    // time (Program::resolve_object_operator_type). It takes precedence over the
    // built-in pointer/arithmetic heuristics so `obj + x` reports the class type,
    // not pointer/int. NULL for ordinary scalar/pointer operators.
    DataDef *resolved_type;
    TokenOperator() : TokenBase() { left = NULL; right = NULL; resolved_type = NULL; _datatype = &ddINT; }
    TokenOperator(int t) : TokenBase(t) { left = NULL; right = NULL; resolved_type = NULL; _datatype = &ddINT; }
    void set_resolved_type(DataDef *d) { resolved_type = d; }
    virtual DataDef *datadef() const override
    { return resolved_type ? resolved_type : (_datatype ? _datatype : &ddVOID); }
    virtual TokenBase *clone() { TokenOperator *to = new TokenOperator(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual int64_t ival() const { return 0; }
    virtual size_t argc() const { return 2; }
    virtual bool is_operator() const override { return true; }
    virtual inline TokenType type()     const { return TokenType::ttOperator;     }
    virtual inline TokenID   id()       const { return TokenID::tkOperator;       }
    virtual inline TokenAssoc assoc()   const { return TokenAssoc::taLeftToRight; }
    virtual inline int precedence() const  { return 15; } // C Operator Precedence, default to 15 (lowest)
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
    virtual TokenBase *clone() { TokenMultiOp *to = new TokenMultiOp(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenType type() const { return TokenType::ttMultiOp; }
    virtual TokenID   id()   const { return TokenID::tkMultiOp; }
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
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator+ on a class object
	if ( left  && left->datadef()  && left->datadef()->is_pointer()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_pointer() ) return right->datadef();
	if ( left  && left->datadef()  && left->datadef()->is_complex()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_complex() ) return right->datadef();
	return TokenOperator::datadef();
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
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator- on a class object
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
};

// negative operator - (unary minus)
class TokenNeg: public TokenOperator
{
public:
    TokenNeg() : TokenOperator('-') {}
    virtual TokenBase *clone() { TokenNeg *to = new TokenNeg(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenID id() const { return TokenID::tkNeg; }
    virtual inline int precedence() const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
    // Propagate unsigned operand type so -1U is uint32, not ddINT
    virtual DataDef *datadef() const override {
	if ( resolved_type ) return resolved_type;
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
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator* on a class object
	if ( left  && left->datadef()  && left->datadef()->is_complex()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_complex() ) return right->datadef();
	return TokenOperator::datadef();
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
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator/ on a class object
	if ( left  && left->datadef()  && left->datadef()->is_complex()  ) return left->datadef();
	if ( right && right->datadef() && right->datadef()->is_complex() ) return right->datadef();
	return TokenOperator::datadef();
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
};

// increment operator ++
class TokenInc: public TokenMultiOp
{
public:
    TokenInc() : TokenMultiOp("++") {}
    virtual TokenBase *clone() { TokenInc *to = new TokenInc(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenID id() const { return TokenID::tkInc; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	if ( left )  return left->datadef();
	if ( right ) return right->datadef();
	return TokenBase::datadef();
    }
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
};

// decrement operator --
class TokenDec: public TokenMultiOp
{
public:
    TokenDec() : TokenMultiOp("--") {}
    virtual TokenBase *clone() { TokenDec *to = new TokenDec(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenID id() const { return TokenID::tkDec; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	if ( left )  return left->datadef();
	if ( right ) return right->datadef();
	return TokenBase::datadef();
    }
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
};

// assignment operator =
class TokenAssign: public TokenOperator
{
public:
    std::vector<Variable *> multi_vars; // for multi-return: a, b := func()
    TokenAssign() : TokenOperator('=') {}
    virtual TokenBase *clone() { TokenAssign *to = new TokenAssign(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const { return TokenID::tkAssign; }
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
};

// assignment operator += (assignment by sum)
class TokenAddEq: public TokenMultiOp
{
public:
    TokenAddEq() : TokenMultiOp("+=") {}
    virtual TokenID id() const { return TokenID::tkAddEq; }
    virtual TokenBase *clone() { return new TokenAddEq(); }
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
    virtual TokenBase *clone() { TokenBnot *to = new TokenBnot(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    // Propagate operand type so ~0U is uint32, not the default ddINT.
    virtual DataDef *datadef() const override {
	if ( resolved_type ) return resolved_type;
	if ( right && right->datadef() && right->datadef()->is_integer() && right->datadef() != &ddINT )
	    return right->datadef();
	return TokenOperator::datadef();
    }
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
};

// logical not operator !
class TokenLnot: public TokenOperator
{
public:
    TokenLnot() : TokenOperator('!') {}
    virtual TokenID id() const { return TokenID::tkLnot; }
    virtual TokenBase *clone() { TokenLnot *to = new TokenLnot(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual inline int precedence()   const { return 2; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
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
    TokenBase *condition;
    TokenBase *true_expr;
    TokenBase *false_expr;
    TokenTerQ() : TokenOperator('?'), condition(NULL), true_expr(NULL), false_expr(NULL) {}
    virtual TokenID id() const { return TokenID::tkTerQ; }
    virtual TokenBase *clone() { return new TokenTerQ(); }
    virtual inline int precedence()   const { return 13; }
    virtual inline TokenAssoc assoc() const { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const { return 1; }
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

// comparison operator !== (not exactly equal to) — !(===)
class Token3NotEq: public TokenMultiOp
{
public:
    Token3NotEq() : TokenMultiOp("!==") {}
    virtual TokenID id() const { return TokenID::tk3NotEq; }
    virtual TokenBase *clone() { return new Token3NotEq(); }
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
// evaluates to either -1 (<), 0 (=), or 1 (>)
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
};

class TokenInt: public TokenBase
{
public:
    std::string source_text;   // original literal text (hex, suffixes)
    // Wide-value handle (P0 slice 2): non-zero when the constant does not fit
    // 64 bits — the full value lives in Program::valpool (madc::dis::value_pool)
    // under this handle. _token / ival() remain the TRUNCATING low-64-bit view
    // (gcc canon: a too-large literal warns + truncates; wide values otherwise
    // arise from >64-bit constant folding).
    uint32_t wide_handle = 0;
    TokenInt() : TokenBase()            { _datatype = &ddINT; }
    TokenInt(int64_t v) : TokenBase(v) { _datatype = &ddINT; }
    TokenInt(int64_t v, const std::string &src) : TokenBase(v), source_text(src) { _datatype = &ddINT; }
    virtual int64_t ival() const        { return _token; }
    // Semantically wide only when the constant's own TYPE is 16 bytes
    // (ddINT128/ddUINT128, set by the parser's fold); see TokenBase::wival.
    virtual madc_wide_int wival() const override
    {
	if ( wide_handle && _active_valpool && _datatype && _datatype->size == 16 )
	    return (madc_wide_int)(((madc_wide_uint)_active_valpool->hi64(wide_handle) << 64)
				   | _active_valpool->lo64(wide_handle));
	return _token;
    }
    virtual double dval() const    { return (double)_token; }
    virtual TokenType type() const { return TokenType::ttInteger; }
    virtual TokenID   id()   const { return TokenID::tkInt; }
    virtual TokenBase *clone()     { auto *c = new TokenInt(_token); c->source_text = source_text; c->_datatype = _datatype; c->wide_handle = wide_handle; return c; }
    virtual bool is_constant() const override { return true; }
    virtual void setDataType(DataDef *d) { if (d && (d->is_integer() || d->is_complex())) _datatype = d; }
};

// A C++ null-pointer constant: an integer LITERAL of value zero ([conv.ptr]).
// Feeds score_arg_to_param's arg_is_zero_literal at every ranking layer that
// can see the argument token. nullptr is already pointer-typed (TokenNullptr)
// and binds pointer parameters natively.
inline bool is_zero_integer_literal(const TokenBase *t)
{
    return t && t->id() == TokenID::tkInt && t->ival() == 0;
}

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

// dynamic_cast<T*>(e) — RTTI down/cross-cast (S5). target_type is the pointee
// class T; operand is the source expression. Lowered to libstdc++ __dynamic_cast
// in cir_builder. _datatype is set to T* at parse time.
class TokenDynamicCast: public TokenBase
{
public:
    DataDef   *target_type;   // pointee class T
    bool       target_is_ptr; // T* form (the only supported form)
    TokenBase *operand;       // the (e) sub-expression
    TokenDynamicCast() : TokenBase(), target_type(NULL), target_is_ptr(false), operand(NULL) {}
    virtual TokenID id() const { return TokenID::tkDynamicCast; }
    virtual TokenBase *clone() { return new TokenDynamicCast(*this); }
};

// typeid(e) / typeid(T) — RTTI query (S5). static_type set for the type form;
// operand set for the expression form. Yields const std::type_info& (modeled as
// a std::type_info* in cir_builder).
class TokenTypeid: public TokenBase
{
public:
    DataDef   *static_type; // non-NULL for typeid(Type)
    TokenBase *operand;     // non-NULL for typeid(expr)
    TokenTypeid() : TokenBase(), static_type(NULL), operand(NULL) {}
    virtual TokenID id() const { return TokenID::tkTypeid; }
    virtual TokenBase *clone() { return new TokenTypeid(*this); }
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
};

class TokenReal: public TokenBase
{
protected:
    double _val;
public:
    std::string source_text;   // original literal text (suffixes like i, iF, f, L)
    TokenReal() : TokenBase()         { _val = 0; _datatype = &ddDOUBLE; }
    TokenReal(double v) : TokenBase() { _val = v; _datatype = &ddDOUBLE; }
    virtual int64_t ival() const      { return (int64_t)_val; }
    virtual double dval() const       { return _val;      }
    virtual TokenType type() const    { return TokenType::ttReal; }
    virtual TokenID   id()   const    { return TokenID::tkReal;   }
    virtual TokenBase *clone()        { auto *c = new TokenReal(_val); c->source_text = source_text; return c; }
    virtual bool is_constant() const override { return true; }
    virtual bool is_real()     const override { return true; }
    virtual void setDataType(DataDef *d) { if (d && (d->is_real() || d->is_complex())) _datatype = d; }
};

// string based tokens

// basic identifier
class TokenIdent: public TokenBase
{
public:
    // Interning Step 4: the per-token `std::string str` is GONE from the identifier
    // base. The spelling lives in the interned pool (rec.spelling_id, resolved via
    // _active_strpool). The ctors intern at construction so every `new TokenIdent(name)`
    // call site keeps working untouched. Content subclasses (TokenStr/TokenREM/
    // TokenKeyword) reintroduce their own `str` and override spelling() — their bytes
    // are mutable/large/embedded-NUL and are NOT identifier spellings.
    TokenIdent() { _datatype = &ddCHARptr; }
    TokenIdent(const std::string &s) { _datatype = &ddCHARptr; if (_active_strpool) rec.spelling_id = _active_strpool->intern(s); }
    TokenIdent(const char *s)        { _datatype = &ddCHARptr; if (_active_strpool && s) rec.spelling_id = _active_strpool->intern(s); }
    // The spelling accessor: interned bytes for this token's spelling_id. VIRTUAL so
    // content subclasses return their retained `str` (NUL-safe) instead of the pool.
    virtual const char *spelling() const {
	return (rec.spelling_id && _active_strpool) ? _active_strpool->c_str(rec.spelling_id) : "";
    }
    bool spelling_is(const char *s) const   { return !std::strcmp(spelling(), s); }
    bool spelling_empty() const             { return !*spelling(); }
    virtual size_t spelling_len() const {
	return (rec.spelling_id && _active_strpool) ? _active_strpool->length(rec.spelling_id) : 0;
    }
    virtual TokenType type() const { return TokenType::ttIdentifier; }
    virtual TokenID   id()   const { return TokenID::tkIdent; }
    virtual TokenBase *clone()     { TokenIdent *t = new TokenIdent(); t->rec.spelling_id = rec.spelling_id; return t; }
    virtual void setDataType(DataDef *d) { if (d) _datatype = d; }
};

// quoted string. `str` holds the literal CONTENT (embedded NULs, mutated by the
// wide-string conversion) — NOT an identifier spelling — so it is retained here and
// spelling() overrides the pool path (interning Step 4). The base ctor still interns
// the initial (NUL-free) bytes into spelling_id for the POD record.
class TokenStr: public TokenIdent
{
public:
    std::string str;
    bool wide;
    TokenStr() : wide(false) {}
    TokenStr(const char *k, bool w = false) : TokenIdent(k), str(k ? k : ""), wide(w) {}
    TokenStr(std::string k, bool w = false) : TokenIdent(k), str(k), wide(w) {}
    virtual const char *spelling() const override { return str.c_str(); }
    virtual size_t spelling_len() const override { return str.size(); }
    virtual int64_t ival() const   { return atol(str.c_str()); }
    virtual bool is_constant() const override { return true; }
    virtual TokenType type() const { return TokenType::ttString; }
    virtual TokenID   id()   const { return TokenID::tkStr; }
    virtual TokenBase *clone()     { return new TokenStr(str, wide); }
};

// comment. `str` holds the comment CONTENT (arbitrary text) — retained here.
class TokenREM: public TokenIdent
{
public:
    std::string str;
    TokenREM() {}
    TokenREM(const char *k) : TokenIdent(k), str(k ? k : "") {}
    TokenREM(std::string k) : TokenIdent(k), str(k) {}
    virtual const char *spelling() const override { return str.c_str(); }
    virtual size_t spelling_len() const override { return str.size(); }
    virtual bool is_constant() const override { return true; }
    virtual TokenType type() const { return TokenType::ttComment; }
    virtual TokenID   id()   const { return TokenID::tkREM; }
    virtual TokenBase *clone()     { return new TokenREM(str); }
};

// keyword tokens. `str` retained (keyword spelling, static; also interned into
// spelling_id by the base ctor). Kept here so contextual-keyword matching / clone
// stay on the concrete spelling without a pool round-trip.
class TokenKeyword: public TokenIdent
{
public:
    std::string str;
    TokenKeyword(const char *k) : TokenIdent(k), str(k ? k : "") {}
    TokenKeyword(std::string k) : TokenIdent(k), str(k) {}
    virtual const char *spelling() const override { return str.c_str(); }
    virtual size_t spelling_len() const override { return str.size(); }
    virtual TokenType type() const { return TokenType::ttKeyword; }
//  virtual TokenBase *clone(){ return new TokenKeyword(str); }
    virtual TokenBase *clone(){ return this; }
    virtual TokenBase *parse(Program &) { return NULL; }
};

// A reserved C++ keyword that has no dedicated dispatch token: it is reserved
// (so it is NOT treated as a bare identifier) but the parser recognizes it by
// SPELLING (contextual_identifier_name), the same way `sizeof`/`decltype`/the
// named casts are already handled. A prototype instance per spelling lives in
// keyword_map; `str` (from TokenIdent) carries the spelling, and `id()` is
// always tkCPPKEYWORD, so a fresh leaf copy is fully equivalent.
//
// clone() MUST return an INDEPENDENT copy (like TokenIdent), NOT the shared
// keyword_map prototype. The base TokenKeyword::clone() returns `this` to
// preserve dispatch SUBTYPES (TokenDO/TokenIF/…); TokenCppKeyword has no such
// subtypes, so `new TokenCppKeyword(str)` reproduces it exactly. Returning the
// shared prototype aliased every `constexpr`/`sizeof`/… occurrence into ONE
// mutable object that the lexer then re-stamped per use (line/column/file) and
// that a retained fn-template decl borrowed — so deleting any one occurrence
// freed it for ALL borrowers (a use-after-free crashing partial-ordering of
// std::forward's overloads on `vector<T*>`; see feature-drops-audit row 6).
class TokenCppKeyword: public TokenKeyword
{
public:
    TokenCppKeyword(const char *k) : TokenKeyword(k) {}
    TokenCppKeyword(const std::string &k) : TokenKeyword(k) {}
    virtual TokenID id() const { return TokenID::tkCPPKEYWORD; }
    virtual TokenBase *clone() { return new TokenCppKeyword(str); }
    // Ignored declaration-specifiers (constexpr/consteval/constinit) consume
    // themselves and continue parsing the declaration they qualify; any other
    // reserved keyword reaching here is an expression leader.
    virtual TokenBase *parse(Program &);
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
};

// `name:` — label statement (function-scoped). Binds the enclosing
// function's Program::label_map[name] at codegen time.
class TokenLabel: public TokenBase
{
public:
    std::string name;
    // The statement the label prefixes (C grammar: `label : statement`).
    // A label is not a standalone statement — it names the statement that
    // follows it. Carrying it here lets every statement context (compound
    // block, switch case body, if/while/for body) emit the labeled statement
    // uniformly, instead of only the compound-block path re-associating labels.
    TokenBase *labeled;
    TokenLabel(const std::string &n) : name(n), labeled(NULL) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual TokenBase *clone()
    {
	TokenLabel *t = new TokenLabel(name);
	if ( labeled )
	    t->labeled = labeled->clone();
	return t;
    }
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
};
class TokenSWITCH: public TokenKeyword
{
public:
    TokenBase *init_stmt;                      // C++17 `switch (init; expr)` init-statement (NULL when absent)
    TokenBase *expression;                     // switch(expr)
    std::vector<TokenCASE *> cases;            // case entries
    TokenCASE *defaultcase;                    // default entry (reuses TokenCASE with value=NULL)
    int default_index;                         // source-order position of default among cases (-1 if none)
    std::vector<TokenBase *> pre_case_stmts;   // declarations before the first case label (C allows them)
    TokenSWITCH() : TokenKeyword("switch"), init_stmt(NULL), expression(NULL), defaultcase(NULL), default_index(-1) {}
    virtual TokenID id() const { return TokenID::tkSWITCH; }
    virtual TokenBase *clone() { return new TokenSWITCH(); }
    virtual TokenBase *parse(Program &);
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

class TokenNAMESPACE:public TokenKeyword { public: TokenNAMESPACE() : TokenKeyword("namespace") {} virtual TokenID id() const { return TokenID::tkNAMESPACE; } virtual TokenBase *clone() { return (TokenBase*)new TokenNAMESPACE(); } virtual TokenBase *parse(Program &); };

class TokenUSING: public TokenKeyword
{
public:
    TokenUSING() : TokenKeyword("using") {}
    virtual TokenID id() const { return TokenID::tkUSING; }
    virtual TokenBase *clone() { return (TokenBase*)new TokenUSING(); }
    virtual TokenBase *parse(Program &);
};

// C++ `friend` declaration specifier. Only valid leading a member declaration
// inside a class/struct body (the struct/class member parsers intercept it on
// the tkFRIEND token); a standalone parse is a misplaced-friend error.
class TokenFRIEND: public TokenKeyword
{
public:
    TokenFRIEND() : TokenKeyword("friend") {}
    virtual TokenID id() const { return TokenID::tkFRIEND; }
    virtual TokenBase *clone() { return (TokenBase*)new TokenFRIEND(); }
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

// STL container keywords are gone: std::map/set/list are header-defined madc
// templates (include/madc/map, include/madc/set), instantiated through the
// class model — not lexer keywords.

// `template<typename T> class Name { ... }` — capture the definition for
// Borland-model instantiation. parse() captures (typeparams, class-body token
// range) into Program::template_map without parsing the body (T is unbound);
// `Name<ConcreteType>` later clones+substitutes+re-parses it as a concrete class.
// See docs/plans/2026-05-30-template-instantiation.md.
class TokenTEMPLATE: public TokenKeyword { public: TokenTEMPLATE() : TokenKeyword("template") {} virtual TokenID id() const { return TokenID::tkTEMPLATE; } virtual TokenBase *clone() { return new TokenTEMPLATE(); } virtual TokenBase *parse(Program &); };

// new / delete — heap allocation with constructor/destructor calls
class TokenNEW: public TokenKeyword
{
public:
    DataDefCLASS *alloc_class;
    std::vector<TokenBase *> ctor_args;
    // Placement new: `new (placement) Type(args)` constructs at the given
    // address instead of allocating. `placement` is the address expression
    // (NULL for ordinary `new`); `alloc_type` is the constructed type when it
    // is not a class (string / scalar), with alloc_class still used for classes.
    TokenBase *placement;
    DataDef *alloc_type;
    TokenBase *array_size;	// `new T[n]` — the element count expr (NULL for scalar new)
    TokenNEW() : TokenKeyword("new") { alloc_class = NULL; placement = NULL; alloc_type = NULL; array_size = NULL; }
    virtual TokenID id() const { return TokenID::tkNEW; }
    virtual TokenBase *clone() { return new TokenNEW(); }
    virtual TokenBase *parse(Program &);
};
class TokenDELETE: public TokenKeyword
{
public:
    TokenBase *expr;
    DataDefCLASS *del_class;
    bool is_array;	// `delete[]` (array delete) vs scalar `delete`
    TokenDELETE() : TokenKeyword("delete") { expr = NULL; del_class = NULL; is_array = false; }
    virtual TokenID id() const { return TokenID::tkDELETE; }
    virtual TokenBase *clone() { return new TokenDELETE(); }
    virtual TokenBase *parse(Program &);
};

// Functional-construction temporary: `T(args)` in expression position constructs
// an anonymous object of class T. CirBuilder lowers it (object_call_temp_addr) to a
// scope-local cleanup-tagged temp + class_ctor_call, yielding the temp as a class
// rvalue (same materialization path as a by-value-returning call). The general C++
// feature — no per-class machinery; works for any class, including header-defined
// std:: classes resolved through the keystone.
class TokenObjTemp: public TokenBase
{
public:
    DataDefCLASS *obj_class;
    std::vector<TokenBase *> ctor_args;
    TokenObjTemp(DataDefCLASS *c) : TokenBase() { obj_class = c; _datatype = (DataDef *)c; }
    virtual TokenID id() const { return TokenID::tkObjTemp; }
    virtual DataDef *datadef() const { return (DataDef *)obj_class; }
    virtual TokenBase *clone() { return new TokenObjTemp(*this); }
};

// Explicit / pseudo destructor call: `obj.~T()` or `ptr->~T()`. Built by the
// expression parser when a `~` follows a `.`/`->`. `obj` is the lhs expression
// (an object for `.`, a pointer for `->`); `dtor_class` is the named type when
// it is a class with a destructor, NULL for a trivial/scalar type (then the
// call is a no-op — `obj` is still evaluated for side effects). CirBuilder
// emits the complete-destructor call (no free, unlike `delete`).
class TokenExplicitDtor: public TokenBase
{
public:
    TokenBase *obj;
    DataDefCLASS *dtor_class;
    bool is_arrow;
    TokenExplicitDtor(TokenBase *o, DataDefCLASS *c, bool arrow)
	: TokenBase(), obj(o), dtor_class(c), is_arrow(arrow) { _datatype = &ddVOID; }
    virtual TokenID id() const { return TokenID::tkExplicitDtor; }
    virtual DataDef *datadef() const { return &ddVOID; }
    virtual TokenBase *clone() { return new TokenExplicitDtor(*this); }
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
};

class TokenBREAK: public TokenKeyword
{
public:
    TokenBREAK() : TokenKeyword("break") {}
    virtual TokenID id() const { return TokenID::tkBREAK; }
    virtual TokenBase *clone() { return new TokenBREAK(); }
    virtual TokenBase *parse(Program &pgm) { return this; }
};

class TokenCONT: public TokenKeyword
{
public:
    TokenCONT() : TokenKeyword("continue") {}
    virtual TokenID id() const { return TokenID::tkCONT;  }
    virtual TokenBase *clone() { return new TokenCONT();  }
    virtual TokenBase *parse(Program &pgm) { return this; }
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
    // C++17 init-statement: `if (init-statement; condition) ...`. The
    // init-statement (a simple-declaration or expression-statement) runs
    // before the condition and shares its scope; NULL when absent.
    TokenBase *init_stmt;
    TokenBase *condition;
    TokenBase *condition_decl;
    TokenBase *statement;
    TokenBase *elsestmt;
    TokenIF() : TokenKeyword("if") { init_stmt = condition = condition_decl = statement = elsestmt = NULL; }
    virtual TokenBase *parse(Program &);
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
    bool elem_is_ref;	// `for (T& v : c)` — loop var aliases the element (mutates source)
    TokenFOREACH() : TokenKeyword("for") { elemtype = NULL; elemvar = NULL; container = statement = NULL; elem_is_ref = false; }
    virtual TokenID id() const { return TokenID::tkFOR; }
    virtual TokenBase *clone() { return new TokenFOREACH(); }
};


#endif // __TOKENS_H
