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

// DataDef is not forward-declarable here: TokenBase::datadef() returns
// `&ddVOID` and usual_arithmetic_result() reads DataDef's members, so this
// header needs the DEFINITION, not a declaration. datadef.h includes nothing
// from this header, so there is no cycle — and without this include the header
// only compiled as a fragment of an including TU's order (g++ -fsyntax-only:
// "'DataDef' does not name a type"), which is what madc's own self-host lane
// reported against it.
#include "datadef.h"

// forward declaration
class Program;

// Kind-accessor forward declarations (TokenBase::as_*() below). Classes not
// defined in this header live in datatokens.h / madc.h.
class TokenOperator;
class TokenAssign;
class TokenTerQ;
class TokenDynamicCast;
class TokenTypeid;
class TokenTypeQuery;
class TokenReal;
class TokenIdent;
class TokenStr;
class TokenGOTO;
class TokenLabel;
class TokenTRY;
class TokenTHROW;
class TokenSWITCH;
class TokenMatch;
class TokenNEW;
class TokenDELETE;
class TokenObjTemp;
class TokenExplicitDtor;
class TokenIF;
class TokenRETURN;
class TokenDO;
class TokenWHILE;
class TokenFOR;
class TokenFOREACH;
class TokenVar;
class TokenCpnd;
class TokenFunc;
class TokenInt;
class TokenDecl;
class TokenCallFunc;
class TokenMember;
class TokenCallMethod;
class TokenSubscript;
class TokenSubscriptExpr;
class TokenTypedefDecl;
class TokenStructLit;
class TokenPackExpansion;
class TokenAddrOf;
class TokenAddrExpr;
class TokenDeref;
class TokenDerefExpr;
class TokenDerefStep;
class TokenCast;

enum class TokenType {
//	0	1	 2	3	4	  5		6	  7
	ttBase, ttSpace, ttTab, ttEOL, ttComment, ttOperator, ttMultiOp, ttSymbol,
//	8		9	  10	 11	    12	    13		14	    15
	ttIdentifier, ttString, ttChar, ttInteger, ttReal, ttKeyword, ttDataType, ttVariable,
//	16		17	  18		19	  20		21	22	 23
	ttFunction, ttCallFunc, ttStatement, ttCompound, ttDeclare, ttProgram, ttMember, ttCallMethod, ttSubscript,
	ttStructLit,
	ttTypedefDecl,	// typedef declaration (preserves source order in AST)
	ttStructDef,	// standalone struct/union definition (preserves source order in AST)
	ttError		// contained parse error (TokenError) — never translates
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
  tkCPPKEYWORD,               // generic reserved C++ keyword (version-gated); the
			      // spelling distinguishes it. Used for reserved words
			      // that the parser still recognizes by spelling (via
			      // contextual_identifier_name) rather than a dedicated
			      // dispatch token — so they are reserved (not bare
			      // identifiers) without proliferating one class each.
  tkGO, tkYIELD,              // madc-dialect cooperative tasks (MT-1): `go
			      // <call-expr>;` spawn and `yield;`/`yield();`.
			      // CONTEXTUAL statement heads under STD_MADC only
			      // (never keyword_map-reserved — real libstdc++
			      // headers use `yield` as an identifier), claimed
			      // by the UFCS error-shape rule: they fire only
			      // where the statement was otherwise ill-formed.
  tkSCOPE, tkAWAIT,           // madc-dialect structure spellings (MT-5), the
			      // same contextual discipline as tkGO/tkYIELD:
			      // `scope { ... }` structured-concurrency block
			      // and `await <chan-expr>` channel receive.
  tkUnaryPlus                 // unary `+` ([expr.unary.op]/7) — built by the engine in
			      // unary position; the lexer always says tkAdd
};

enum class TokenAssoc {
    taNone, taLeftToRight, taRightToLeft
};

// Highlight classification (madcide AST-2 / IDE-7): what KIND of thing a
// lexed token is, for presentation. The one classifier is
// madc_token_highlight_class (lexer.cpp — the token-vocabulary owner);
// names cross the value boundary via highlight_class_name. A theme (app
// data) maps these names to colours — the compiler never styles.
enum class HighlightClass : unsigned char
{
    hcNone = 0,		// operators / punctuation — unstyled
    hcKeyword,
    hcIdent,
    hcNumber,
    hcString,		// string AND char literals
    hcComment,		// from leading trivia (keep_trivia mode)
    hcType,		// datatype spellings (tkDeclare)
    hcFunction		// an identifier the tree knows as a function name
};

inline const char *highlight_class_name(HighlightClass c)
{
    switch ( c )
    {
	case HighlightClass::hcNone:	 return "none";
	case HighlightClass::hcKeyword:	 return "keyword";
	case HighlightClass::hcIdent:	 return "ident";
	case HighlightClass::hcNumber:	 return "number";
	case HighlightClass::hcString:	 return "string";
	case HighlightClass::hcComment:	 return "comment";
	case HighlightClass::hcType:	 return "type";
	case HighlightClass::hcFunction: return "function";
    }
    return "none";
}

// Error-node vocabulary (error-tolerant parse, 2026-08-25 owner ruling —
// design doc 2026-08-25-madcide-ast-arc-design.md §3.5). One TokenError
// class carries the whole vocabulary; the KIND is data (enum-over-strings).
// Two families:
//   Holes  (Missing*): zero-width, SYNTHESIZED where the grammar required
//          something — the tree stays structurally complete and queryable.
//   Debris (UnexpectedToken, SkippedTokens): REAL source tokens set aside,
//          spellings/positions/trivia retained — the source view stays exact.
// ANY error node present gates translate (Program::error_nodes > 0 refuses
// at cir_translate_guarded — "prevent compilation" is the owner ruling).
// The full vocabulary is declared up front to prevent drift; synthesis
// sites grow into it incrementally (slice A emits only the debris kinds).
enum class ErrorNodeKind : unsigned char
{
    MissingExpression = 0,
    MissingStatement,
    MissingDeclaration,
    MissingIdentifier,
    MissingType,
    MissingToken,
    UnexpectedToken,
    SkippedTokens
};

inline const char *error_node_kind_name(ErrorNodeKind k)
{
    switch ( k )
    {
	case ErrorNodeKind::MissingExpression:  return "MissingExpression";
	case ErrorNodeKind::MissingStatement:   return "MissingStatement";
	case ErrorNodeKind::MissingDeclaration: return "MissingDeclaration";
	case ErrorNodeKind::MissingIdentifier:  return "MissingIdentifier";
	case ErrorNodeKind::MissingType:        return "MissingType";
	case ErrorNodeKind::MissingToken:       return "MissingToken";
	case ErrorNodeKind::UnexpectedToken:    return "UnexpectedToken";
	case ErrorNodeKind::SkippedTokens:      return "SkippedTokens";
    }
    return "ErrorNode";
}


// Token flags
typedef enum : uint16_t { tfBRACKETED	=    1,
			  tfOVERLOADED  =    2,
			  tfSYNTHPOS	=    4,	// minted from synthesized pushback text
						// (macro expansion, __FILE__/__LINE__):
						// line/column name the invocation site,
						// not source bytes of this spelling
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
    // Source EXTENT of a parsed construct (code-graph MCP L3, design §6.6/§9:
    // the statement is the edit unit). Stamped by the PARSER, the one owner
    // (Program::parseStatement's wrapper, parseCompound's '}', parseFunction):
    //   head_tok   — the FIRST source token of the construct (the token
    //                parseStatement was handed); its START is the extent start
    //   end_line / end_column — the END of the LAST consumed token: the static
    //                parse position when the construct finished (a simple
    //                statement's ';', a compound's '}'). Columns are END-
    //                anchored (the byte after the token's last char).
    // NULL / 0 = no extent (a leaf, an expression node, a synthesized token).
    // TokenCpnd's former end_line (the closing-brace line) lives here now.
    // Replaced the never-read, never-written `std::streampos pos`.
    TokenBase *head_tok;
    int end_line;
    int end_column;
    // Flat POD data record (Phase 2). See TokenRec above.
    TokenRec rec;
    // Diagnostic: how many times the parser has CONSUMED this token via
    // nextToken() (a re-read > 1 means backtracking / pushback re-lexing /
    // template re-instantiation touched the same token object). Reported in
    // aggregate by --show-stats; otherwise just one uint per token.
    uint32_t read_count;
    // [lex.ext] ud-suffix. A user-defined-literal is ONE token — `123_w` is
    // not `123` followed by `_w`, and the adjacency that decides it is only
    // knowable at LEX. The suffix interns into the same pool as spelling_id
    // (-> Program::strpool; 0 = this literal carries no ud-suffix), so it
    // costs one uint per token rather than a std::string.
    // NOT in TokenRec: that record is the serialized pop-1 ROM, and widening
    // it changes the frozen-header pack layout. A packed system header that
    // USED a ud-suffix in an expression would therefore lose it — move this
    // into TokenRec (with a pack version bump) if the pack ever round-trips
    // expression bodies.
    uint32_t ud_suffix_id = 0;
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
    TokenBase()           { _token = 0; _datatype = &ddVOID; _flags = 0; file = _parse_file; parent = NULL; line = _parse_line; column = _parse_column; head_tok = NULL; end_line = 0; end_column = 0; read_count = 0; }
    TokenBase(int64_t t)  { _token = t; _datatype = &ddVOID; _flags = 0; file = _parse_file; parent = NULL; line = _parse_line; column = _parse_column; head_tok = NULL; end_line = 0; end_column = 0; read_count = 0; }
    virtual ~TokenBase() {}
    // Every token (and every clone()) allocates from the per-process TokenArena
    // (token_arena.h). operator delete is a no-op: tokens are never individually
    // freed — a `delete tok` still runs the virtual destructor (freeing that
    // token's std::string members) but the arena cell is reclaimed only when the
    // arena is dropped. See docs/plans/2026-06-23-p1-token-arena-implementation-plan.md.
    static void *operator new(std::size_t sz);
    static void  operator delete(void *p);
    virtual TokenBase *clone() { return new TokenBase(_token); }
    // A copy that keeps WHERE the original came from. clone() builds a fresh
    // token, and the constructor stamps it with the CURRENT parse position —
    // right for a macro-expansion replacement (the expansion site is its
    // location; the lexer's clones), wrong for every copy the parser makes of
    // a template pattern, a default argument, or a call argument: those are
    // the same source text from the same place, and an instantiation is
    // attributed to the pattern's definition ([temp.inst]; gcc's instantiated
    // decl carries the pattern's DECL_SOURCE_LOCATION, clang's the template's
    // SourceLocation). A pattern copy stamped with the call site's file read
    // as USER code to the builder's root/library split and was emitted
    // unconditionally — the darwin pack lowering an unselected libc++
    // basic_string constructor instance whose body could not link.
    TokenBase *clone_origin()
    {
	TokenBase *c = clone();
	if ( c )
	{
	    c->file = file;
	    c->line = line;
	    c->column = column;
	    // The ud-suffix is part of the literal's IDENTITY, not its
	    // position — a cloned `123_w` is still `123_w`. Propagated here
	    // (the sanctioned copier) because clone() is per-class.
	    c->ud_suffix_id = ud_suffix_id;
	}
	return c;
    }
    // The [lex.ext] ud-suffix this literal carries, or NULL for the ordinary
    // case. Resolved through the active intern pool, like spelling().
    const char *ud_suffix() const
    {
	return (ud_suffix_id && _active_strpool)
	     ? _active_strpool->c_str(ud_suffix_id) : NULL;
    }
    virtual void set(int64_t c) { _token = c; }
    virtual void setDataType(DataDef *d) { if (d) _datatype = d; }
    virtual void setFlag(tokflag_t f) { _flags |= f; }
    virtual bool is_bracketed() const { return (_flags & tfBRACKETED) ? true : false;  }
    virtual bool is_overloaded() const { return (_flags & tfOVERLOADED) ? true : false; }
    bool is_synthetic_position() const { return (_flags & tfSYNTHPOS) ? true : false; }
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
    // Kind accessors — the O(1) replacement for dynamic_cast<TokenX *> on the
    // hot dispatch paths (the -O2 launch profile put ~9% of a cold start in
    // libstdc++ __dynamic_cast; the token tree's virtual bases make each cast
    // a type_info graph walk). Each class overrides its own accessor with
    // `return this;`, so derived classes INHERIT the override and the closure
    // matches dynamic_cast semantics exactly (TokenMember answers as_var()
    // through TokenCallFunc -> TokenVar the same way dynamic_cast<TokenVar *>
    // succeeds on it). Callers must null-check the RECEIVER; dynamic_cast
    // accepted a null operand, a virtual call does not.
    virtual TokenOperator      *as_operator_tok()   { return NULL; }
    virtual TokenAssign        *as_assign_tok()     { return NULL; }
    virtual TokenTerQ          *as_terq_tok()       { return NULL; }
    virtual TokenDynamicCast   *as_dyncast_tok()    { return NULL; }
    virtual TokenTypeid        *as_typeid_tok()     { return NULL; }
    virtual TokenTypeQuery     *as_typequery_tok()  { return NULL; }
    virtual TokenReal          *as_real_tok()       { return NULL; }
    virtual TokenIdent         *as_ident_tok()      { return NULL; }
    virtual TokenStr           *as_str_tok()        { return NULL; }
    virtual TokenGOTO          *as_goto_tok()       { return NULL; }
    virtual TokenLabel         *as_label_tok()      { return NULL; }
    virtual TokenTRY           *as_try_tok()        { return NULL; }
    virtual TokenTHROW         *as_throw_tok()      { return NULL; }
    virtual TokenSWITCH        *as_switch_tok()     { return NULL; }
    virtual TokenMatch         *as_match_tok()      { return NULL; }
    virtual TokenNEW           *as_new_tok()        { return NULL; }
    virtual TokenDELETE        *as_delete_tok()     { return NULL; }
    virtual TokenObjTemp       *as_objtemp_tok()    { return NULL; }
    virtual TokenExplicitDtor  *as_explicit_dtor_tok() { return NULL; }
    virtual TokenIF            *as_if_tok()         { return NULL; }
    virtual TokenRETURN        *as_return_tok()     { return NULL; }
    virtual TokenDO            *as_do_tok()         { return NULL; }
    // Code-graph MCP L1b confirm-before-build (2026-09-12): `while (x) {}`
    // genuinely lives as a TokenWHILE in the live tree — TokenWHILE::parse()
    // (src/parser.cpp:50090) returns `this` (not lowered to TokenFOR), and
    // it is read back via dynamic_cast<TokenWHILE*> elsewhere
    // (src/parser.cpp:66744, deduce_return_type_from_stmt). No downcast
    // existed for it (unlike as_do_tok/as_for_tok/as_foreach_tok beside it),
    // so the body-graph walker had no O(1) way to reach condition/statement.
    // Added here, mirroring the family.
    virtual TokenWHILE         *as_while_tok()      { return NULL; }
    virtual TokenFOR           *as_for_tok()        { return NULL; }
    virtual TokenFOREACH       *as_foreach_tok()    { return NULL; }
    virtual TokenVar           *as_var_tok()        { return NULL; }
    virtual TokenCpnd          *as_cpnd_tok()       { return NULL; }
    virtual TokenFunc          *as_func_tok()       { return NULL; }
    virtual TokenInt           *as_int_tok()        { return NULL; }
    virtual TokenDecl          *as_decl_tok()       { return NULL; }
    virtual TokenCallFunc      *as_callfunc_tok()   { return NULL; }
    virtual TokenMember        *as_member_tok()     { return NULL; }
    virtual TokenCallMethod    *as_callmethod_tok() { return NULL; }
    virtual TokenSubscript     *as_subscript_tok()  { return NULL; }
    virtual TokenSubscriptExpr *as_subscript_expr_tok() { return NULL; }
    virtual class TokenMemberPtrConst *as_member_ptr_const_tok() { return NULL; }
    virtual class TokenMemberPtrAccess *as_member_ptr_access_tok() { return NULL; }
    virtual TokenTypedefDecl   *as_typedef_decl_tok()   { return NULL; }
    virtual TokenStructLit     *as_struct_lit_tok() { return NULL; }
    virtual TokenPackExpansion *as_pack_expansion_tok() { return NULL; }
    virtual TokenAddrOf        *as_addr_of_tok()    { return NULL; }
    virtual TokenAddrExpr      *as_addr_expr_tok()  { return NULL; }
    virtual TokenDeref         *as_deref_tok()      { return NULL; }
    virtual TokenDerefExpr     *as_deref_expr_tok() { return NULL; }
    virtual TokenDerefStep     *as_deref_step_tok() { return NULL; }
    virtual TokenCast          *as_cast_tok()       { return NULL; }
    // Is this a DEREFERENCE — any of the three nodes Program::build_indirection
    // builds (TokenDeref for a named pointer, TokenDerefStep for `*p++`,
    // TokenDerefExpr otherwise)? The builder picks the kind from the operand's
    // shape; a consumer asking "is this `*x`" must not depend on that choice
    // (a call arm that knew TokenDerefExpr alone lost `(*t)(i)`).
    bool is_indirection()
    { return as_deref_tok() || as_deref_expr_tok() || as_deref_step_tok(); }
};

// whitespace

// plain space
class TokenSpace: public TokenBase
{
public:
    int cnt;
    TokenSpace() : TokenBase(' ') { cnt = 0; }
    TokenSpace(int c) : TokenBase(' ') { cnt = c; }
    virtual TokenBase *clone() override{ return new TokenSpace(cnt); }
    virtual TokenType type() const override { return TokenType::ttSpace; }
    virtual TokenID   id()   const override { return TokenID::tkSpace; }
};

// tab
class TokenTab: public TokenBase
{
public:
    int cnt;
    TokenTab() : TokenBase(9) { cnt = 0; }
    TokenTab(int c) : TokenBase(9) { cnt = c; }
    virtual TokenBase *clone() override{ return new TokenTab(cnt); }
    virtual TokenType type() const override { return TokenType::ttTab; }
    virtual TokenID   id()   const override { return TokenID::tkTab; }
};

// end of line
class TokenEOL: public TokenBase
{
public:
    int cnt;
    TokenEOL() : TokenBase(13) { cnt = 0; }
    TokenEOL(int c) : TokenBase(13) { cnt = c; }
    virtual TokenBase *clone() override{ return new TokenEOL(cnt); }
    virtual TokenType type() const override { return TokenType::ttEOL; }
    virtual TokenID   id()   const override { return TokenID::tkEOL; }
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
    // A comparison or logical operator: its built-in result is a truth value
    // — bool in C++, int in C (Program::resolve_object_operator_type records
    // which, by the language, as the token's own type).
    virtual bool yields_truth_value() const { return false; }
    virtual TokenBase *clone() override { TokenOperator *to = new TokenOperator(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual int64_t ival() const override { return 0; }
    virtual size_t argc() const override { return 2; }
    virtual bool is_operator() const override { return true; }
    virtual inline TokenType type()     const override { return TokenType::ttOperator;     }
    virtual inline TokenID   id()       const override { return TokenID::tkOperator;       }
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
    virtual TokenOperator *as_operator_tok() override { return this; }
};

// multi-symbol operator base class
class TokenMultiOp: public TokenOperator
{
public:
    std::string str;
    TokenMultiOp() : TokenOperator() {}
    TokenMultiOp(const char *s)  : TokenOperator() { str = s; }
    TokenMultiOp(std::string &s) : TokenOperator() { str = s; }
    virtual TokenBase *clone() override { TokenMultiOp *to = new TokenMultiOp(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenType type() const override { return TokenType::ttMultiOp; }
    virtual TokenID   id()   const override { return TokenID::tkMultiOp; }
    virtual inline int precedence() const override { return 16; }
};

// The parse-side type of an arithmetic OPERAND has one owner per step, and
// every operator's datadef() below reads its children through them — never
// through a bare child->datadef() (gated: check-one-operand-promotion.sh).
//  * operand_value_type: the VALUE the operand denotes ([expr]/5). A
//    reference-typed expression answers a DataDefREF and a reference VARIABLE
//    answers the pointer madc lowers it as; both denote the referent.
//    Program::operand_value_datadef composes from it.
//  * promoted_operand_type: that value, cv-unqualified ([conv.lval]), after
//    the integer promotions ([conv.prom], C11 6.3.1.1p2): bool, the character
//    types, the shorts, a bit-field narrower than int and an enum promoting
//    to int answer int; an enum its promoted type; char32_t unsigned int.
//    Every other value (int and wider, reals, pointers, classes, vectors,
//    complex) answers itself.
// Each asks the child's datadef() exactly ONCE (the TokenAdd rule below).
// Defined in parser.cpp: a bit-field's width and a reference variable's flag
// live on TokenMember / TokenVar, which madc.h declares.
DataDef *operand_value_type(TokenBase *operand);
DataDef *promoted_operand_type(TokenBase *operand);
// The integer-promotion core over a TYPE (the enum and character-type rules;
// cir_builder's enum_promotes_to composes from it). NULL-safe.
DataDef *integer_promoted_type(DataDef *dd);

// Integer conversion rank (C11 6.3.1.1p1, [conv.rank]) of a PROMOTED operand
// type: width orders the ranks, except where two integer types share a width —
// the platform `long` (LLP64: 4 bytes, beside int) and the platform `long
// long` (darwin: 8 bytes, beside long) each outrank the other type of their
// width. A width test alone made `i + l` an int on win64 (mingw: long).
static inline int integer_conversion_rank(const DataDef *dd)
{
    return (int)dd->size * 2 + (dd_is_platform_integer(dd) ? 1 : 0);
}

// The unsigned integer type corresponding to a signed one (C11 6.2.5p6) — the
// usual arithmetic conversions' last case, where the signed operand outranks
// the unsigned one but cannot represent all its values (win64 `long` against
// `unsigned int` is `unsigned long`).
static inline DataDef *unsigned_integer_counterpart(DataDef *dd)
{
    if ( dynamic_cast<DataDefPlatformLONG *>(dd) )
	return dd_platform_ulong();
    if ( dynamic_cast<DataDefPlatformLONGLONG *>(dd) )
	return dd_platform_ulonglong();
    if ( dd->size == ddUINT128.size )
	return &ddUINT128;
    if ( dd->size == ddUINT64.size )
	return &ddUINT64;
    return &ddUINT32;
}

// addition operator +
// Usual arithmetic conversions (C11 6.3.1.8) — the parse-side VALUE view
// shared by the binary arithmetic operators' datadef() overrides: a real
// operand wins (wider real first); otherwise the integer rule by RANK
// (integer_conversion_rank) — same signedness, the higher rank; an unsigned
// operand of rank >= the signed one's, the unsigned; a signed one wider than
// the unsigned, the signed; else the signed one's unsigned counterpart. The
// operands arrive PROMOTED
// (promoted_operand_type), so plain int, pointer/function/complex operands
// and NULL children answer NULL and each operator's own arms and the ddINT
// default keep their behavior; the floor tests below stay as the contract.
// (Runtime codegen types via c2mir regardless; this view feeds parse-time
// consumers — _Generic selection, overload ranking, sizeof-of-expression.)
static inline DataDef *usual_arithmetic_result(DataDef *ld, DataDef *rd)
{
    if ( !ld || !rd )
	return NULL;
    if ( ld->is_pointer() || rd->is_pointer()
      || ld->is_function() || rd->is_function()
      || ld->is_complex() || rd->is_complex() )
	return NULL;
    if ( !ld->is_numeric() || !rd->is_numeric() )
	return NULL;
    bool lr = ld->is_real(), rr = rd->is_real();
    if ( lr || rr )
    {
	if ( lr && rr )
	    return ld->size >= rd->size ? ld : rd;
	return lr ? ld : rd;
    }
    const bool lu = ld->is_unsigned(), ru = rd->is_unsigned();
    const int lk = integer_conversion_rank(ld), rk = integer_conversion_rank(rd);
    DataDef *w;
    if ( lu == ru )
	w = rk > lk ? rd : ld;
    else
    {
	DataDef *u = lu ? ld : rd, *sg = lu ? rd : ld;
	if ( (lu ? lk : rk) >= (lu ? rk : lk) )
	    w = u;
	else if ( sg->size > u->size )
	    w = sg;
	else
	    w = unsigned_integer_counterpart(sg);
    }
    if ( w->size < ddINT.size )
	return NULL;	// below the promotion floor: both promote to int
    if ( !w->is_unsigned()
      && integer_conversion_rank(w) == integer_conversion_rank(&ddINT) )
	return NULL;	// plain int — the caller default already
    return w;
}

class TokenAdd: public TokenOperator
{
public:
    TokenAdd() : TokenOperator('+') {}
    virtual TokenBase *clone() override { TokenAdd *to = new TokenAdd(); to->left = left; to->right = right; return to; }
    virtual inline int precedence() const override { return 4; }
    virtual TokenID id() const override { return TokenID::tkAdd; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator+ on a class object
	// Each child's datadef() exactly ONCE: these overrides recurse into
	// operator children, so re-asking per predicate made one query on an
	// N-deep `a+b+c+...` chain cost 4^N (c-testsuite 00205 — a J-
	// interpreter initializer of ~50-add chains — HUNG the compiler;
	// callgrind: 59% TokenAdd::datadef'2). Same rule in every operator
	// override below.
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	if ( ld && ld->is_pointer() ) return ld;
	if ( rd && rd->is_pointer() ) return rd;
	if ( ld && ld->is_complex() ) return ld;
	if ( rd && rd->is_complex() ) return rd;
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// top precedence operator
class TokenPrimary: public TokenOperator
{
public:
    TokenPrimary(int t) : TokenOperator(t) {}
    virtual TokenBase *clone() override { TokenPrimary *to = new TokenPrimary(_token); to->left = left; to->right = right; return to; }
    virtual TokenID id() const override { return TokenID::tkOperator; }
    virtual inline int precedence() const override { return 1; }
};

// substraction operator -
class TokenSub: public TokenOperator
{
public:
    TokenSub() : TokenOperator('-') {}
    virtual TokenBase *clone() override { TokenSub *to = new TokenSub(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const override { return TokenID::tkSub; }
    virtual inline int precedence() const override { return 4; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator- on a class object
	// Children queried ONCE (the TokenAdd exponential-recursion rule).
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	// `p - n` is a pointer; `p - q` (both pointers) is ptrdiff_t.
	if ( ld && ld->is_pointer() )
	{
	    if ( rd && rd->is_pointer() )
		return TokenOperator::datadef();
	    return ld;
	}
	if ( ld && ld->is_complex() ) return ld;
	if ( rd && rd->is_complex() ) return rd;
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// negative operator - (unary minus)
class TokenNeg: public TokenOperator
{
public:
    TokenNeg() : TokenOperator('-') {}
    virtual TokenBase *clone() override { TokenNeg *to = new TokenNeg(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenID id() const override { return TokenID::tkNeg; }
    virtual inline int precedence() const override { return 2; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
    // [expr.unary.op]/8, C11 6.5.3.3: the result is the PROMOTED operand's
    // type — -uc is int, -1U unsigned, -7L long, -2.5 double (overload ranking
    // reads this VALUE view; without the real arm abs(-2.5) once bound the
    // int overload). A complex or vector operand is its own result.
    virtual DataDef *datadef() const override {
	if ( resolved_type ) return resolved_type;
	DataDef *rd = promoted_operand_type(right);
	if ( rd && (rd->is_complex() || rd->is_simd()) )
	    return rd;
	if ( DataDef *ua = usual_arithmetic_result(rd, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// unary plus + ([expr.unary.op]/7, C11 6.5.3.3): the PROMOTED operand, an
// rvalue — never the operand itself. A class operand's operator+() types
// through resolved_type, and an array or function operand's decay is set
// there at reduce time (resolve_object_operator_type). The lexer always
// says TokenAdd for '+'; the engine builds this in unary position, as it
// turns a TokenNeg into a TokenSub in binary position.
class TokenUnaryPlus: public TokenOperator
{
public:
    TokenUnaryPlus() : TokenOperator('+') {}
    virtual TokenBase *clone() override { TokenUnaryPlus *to = new TokenUnaryPlus(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenID id() const override { return TokenID::tkUnaryPlus; }
    virtual inline int precedence() const override { return 2; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
    virtual DataDef *datadef() const override {
	if ( resolved_type ) return resolved_type;
	DataDef *rd = promoted_operand_type(right);
	if ( rd && (rd->is_pointer() || rd->as_fptr_dd() || rd->is_complex()
		 || rd->is_simd()) )
	    return rd;
	if ( DataDef *ua = usual_arithmetic_result(rd, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// multiply operator *
class TokenMul: public TokenOperator
{
public:
    TokenMul() : TokenOperator('*') {}
    virtual TokenBase *clone() override { TokenMul *to = new TokenMul(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const override { return TokenID::tkMul; }
    virtual inline int precedence() const override { return 3; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator* on a class object
	// Children queried ONCE (the TokenAdd exponential-recursion rule).
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	if ( ld && ld->is_complex() ) return ld;
	if ( rd && rd->is_complex() ) return rd;
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// divide operator /
class TokenDiv: public TokenOperator
{
public:
    TokenDiv() : TokenOperator('/') {}
    virtual TokenBase *clone() override { TokenDiv *to = new TokenDiv(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const override { return TokenID::tkDiv; }
    virtual inline int precedence() const override { return 3; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator/ on a class object
	// Children queried ONCE (the TokenAdd exponential-recursion rule).
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	if ( ld && ld->is_complex() ) return ld;
	if ( rd && rd->is_complex() ) return rd;
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// modulo / remainder operator %
class TokenMod: public TokenOperator
{
public:
    TokenMod() : TokenOperator('%') {}
    virtual TokenBase *clone() override { TokenMod *to = new TokenMod(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const override { return TokenID::tkMod; }
    virtual inline int precedence() const override { return 3; }
    // C11 6.5.5, [expr.mul]: the usual arithmetic conversions, TokenMul's rule.
    // Without it `%` answered the TokenOperator default — `l % 3` on a long
    // measured 4 bytes and `auto r = l % m` truncated.
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;   // overloaded operator% on a class object
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// increment operator ++
class TokenInc: public TokenMultiOp
{
public:
    TokenInc() : TokenMultiOp("++") {}
    virtual TokenBase *clone() override { TokenInc *to = new TokenInc(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenID id() const override { return TokenID::tkInc; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	if ( left )  return operand_value_type(left);
	if ( right ) return operand_value_type(right);
	return TokenBase::datadef();
    }
    virtual inline int precedence()   const override { return 2; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
};

// decrement operator --
class TokenDec: public TokenMultiOp
{
public:
    TokenDec() : TokenMultiOp("--") {}
    virtual TokenBase *clone() override { TokenDec *to = new TokenDec(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual TokenID id() const override { return TokenID::tkDec; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	if ( left )  return operand_value_type(left);
	if ( right ) return operand_value_type(right);
	return TokenBase::datadef();
    }
    virtual inline int precedence()   const override { return 2; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
};

// assignment operator =
class TokenAssign: public TokenOperator
{
public:
    std::vector<Variable *> multi_vars; // for multi-return: a, b := func()
    TokenAssign() : TokenOperator('=') {}
    virtual TokenBase *clone() override { TokenAssign *to = new TokenAssign(); to->left = left; to->right = right; return to; }
    virtual TokenID id() const override { return TokenID::tkAssign; }
    virtual DataDef *datadef() const override
    {
	// An assignment-as-expression evaluates to the assigned LHS
	// value, so its type is the LHS's type — required for
	// `*(end = ptr + N)` where `end` is `char *`.
	// Child queried ONCE (the TokenAdd exponential-recursion rule).
	DataDef *ld = operand_value_type(left);
	if ( ld ) return ld;
	return TokenOperator::datadef();
    }
    virtual inline int precedence()   const override { return 14; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual TokenAssign *as_assign_tok() override { return this; }
};

// A compound assignment `a @= b` ([expr.ass]/1 and /7, C11 6.5.16p3): its
// type is the LEFT operand's, as `=`'s is (TokenAssign reads the same
// operand_value_type); a class's operator@= keeps its declared return
// (resolved_type). The ten operators below declared no type of their own and
// answered the operator default, int — `*(p += 2)` was refused.
class TokenCompoundAssign: public TokenMultiOp
{
public:
    TokenCompoundAssign(const char *s) : TokenMultiOp(s) {}
    virtual inline int precedence()   const override { return 14; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	if ( DataDef *ld = operand_value_type(left) ) return ld;
	return TokenOperator::datadef();
    }
};

// assignment operator += (assignment by sum)
class TokenAddEq: public TokenCompoundAssign
{
public:
    TokenAddEq() : TokenCompoundAssign("+=") {}
    virtual TokenID id() const override { return TokenID::tkAddEq; }
    virtual TokenBase *clone() override { return new TokenAddEq(); }
};

// assignment operator -= (assignment by difference)
class TokenSubEq: public TokenCompoundAssign
{
public:
    TokenSubEq() : TokenCompoundAssign("-=") {}
    virtual TokenID id() const override { return TokenID::tkSubEq; }
    virtual TokenBase *clone() override { return new TokenSubEq(); }
};

// assignment operator *= (assignment by product)
class TokenMulEq: public TokenCompoundAssign
{
public:
    TokenMulEq() : TokenCompoundAssign("*=") {}
    virtual TokenID id() const override { return TokenID::tkMulEq; }
    virtual TokenBase *clone() override { return new TokenMulEq(); }
};

// assignment operator /= (assignment by quotient)
class TokenDivEq: public TokenCompoundAssign
{
public:
    TokenDivEq() : TokenCompoundAssign("/=") {}
    virtual TokenID id() const override { return TokenID::tkDivEq; }
    virtual TokenBase *clone() override { return new TokenDivEq(); }
};

// assignment operator %= (assignment by remainder)
class TokenModEq: public TokenCompoundAssign
{
public:
    TokenModEq() : TokenCompoundAssign("%=") {}
    virtual TokenID id() const override { return TokenID::tkModEq; }
    virtual TokenBase *clone() override { return new TokenModEq(); }
};

// assignment operator <<= (assignment by bitwise left shift)
class TokenBSLEq: public TokenCompoundAssign
{
public:
    TokenBSLEq() : TokenCompoundAssign("<<=") {}
    virtual TokenID id() const override { return TokenID::tkBSLEq; }
    virtual TokenBase *clone() override { return new TokenBSLEq(); }
};

// assignment operator >>= (assignment by bitwise right shift)
class TokenBSREq: public TokenCompoundAssign
{
public:
    TokenBSREq() : TokenCompoundAssign(">>=") {}
    virtual TokenID id() const override { return TokenID::tkBSREq; }
    virtual TokenBase *clone() override { return new TokenBSREq(); }
};

// assignment operator &= (assignment by bitwise and)
class TokenBandEq: public TokenCompoundAssign
{
public:
    TokenBandEq() : TokenCompoundAssign("&=") {}
    virtual TokenID id() const override { return TokenID::tkBandEq; }
    virtual TokenBase *clone() override { return new TokenBandEq(); }
};

// assignment operator |= (assignment by bitwise or)
class TokenBorEq: public TokenCompoundAssign
{
public:
    TokenBorEq() : TokenCompoundAssign("|=") {}
    virtual TokenID id() const override { return TokenID::tkBorEq; }
    virtual TokenBase *clone() override { return new TokenBorEq(); }
};

// assignment operator ^= (assignment by bitwise xor)
class TokenXorEq: public TokenCompoundAssign
{
public:
    TokenXorEq() : TokenCompoundAssign("^=") {}
    virtual TokenID id() const override { return TokenID::tkXorEq; }
    virtual TokenBase *clone() override { return new TokenXorEq(); }
};

// overload function operator ()
class TokenFuncOp: public TokenMultiOp
{
public:
    TokenFuncOp() : TokenMultiOp("()") {}
    virtual TokenID id() const override { return TokenID::tkFuncOp; }
    virtual TokenBase *clone() override { return new TokenFuncOp(); }
    virtual inline int precedence() const override { return 1; }
};

// overload array operator []
class TokenArrayOp: public TokenMultiOp
{
public:
    TokenArrayOp() : TokenMultiOp("[]") {}
    virtual TokenID id() const override { return TokenID::tkArrayOp; }
    virtual TokenBase *clone() override { return new TokenArrayOp(); }
    virtual inline int precedence() const override { return 1; }
};

// bitwise not operator ~
class TokenBnot: public TokenOperator
{
public:
    TokenBnot() : TokenOperator('~') {}
    virtual TokenID id() const override { return TokenID::tkBnot; }
    virtual TokenBase *clone() override { TokenBnot *to = new TokenBnot(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    // [expr.unary.op]/10, C11 6.5.3.3: the PROMOTED operand's type — ~uc is
    // int, ~0U unsigned; a complex operand is its own result (~z is the
    // conjugate, GNU), and so is a vector.
    virtual DataDef *datadef() const override {
	if ( resolved_type ) return resolved_type;
	DataDef *rd = promoted_operand_type(right);
	if ( rd && (rd->is_complex() || rd->is_simd()) )
	    return rd;
	if ( DataDef *ua = usual_arithmetic_result(rd, rd) ) return ua;
	return TokenOperator::datadef();
    }
    virtual inline int precedence()   const override { return 2; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
};

// logical not operator !
class TokenLnot: public TokenOperator
{
public:
    TokenLnot() : TokenOperator('!') {}
    virtual TokenID id() const override { return TokenID::tkLnot; }
    virtual TokenBase *clone() override { TokenLnot *to = new TokenLnot(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual inline int precedence()   const override { return 2; }
    virtual bool yields_truth_value() const override { return true; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
};

// bitwise and operator &
class TokenBand: public TokenOperator
{
public:
    TokenBand() : TokenOperator('&') {}
    virtual TokenID id() const override { return TokenID::tkBand; }
    virtual TokenBase *clone() override { return new TokenBand(); }
    virtual inline int precedence() const override { return 8; }
    // C11 6.5.10: the usual arithmetic conversions type a bitwise operator
    // exactly as they type the additive ones (TokenAdd's rule; children
    // queried ONCE) — the wider / unsigned integer, and a SIMD operand's
    // vector. The bare TokenOperator default (`int`) typed `(a & b)[i]` as a
    // scalar, so the `[` fell through to the lambda introducer.
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// logical and operator &&
class TokenLand: public TokenMultiOp
{
public:
    TokenLand() : TokenMultiOp("&&") {}
    virtual TokenID id() const override { return TokenID::tkLand; }
    virtual TokenBase *clone() override { return new TokenLand(); }
    virtual inline int precedence() const override { return 11; }
    virtual bool yields_truth_value() const override { return true; }
};

// bitwise or operator | (inclusive or)
class TokenBor: public TokenOperator
{
public:
    TokenBor() : TokenOperator('|') {}
    virtual TokenID id() const override { return TokenID::tkBor; }
    virtual TokenBase *clone() override { return new TokenBor(); }
    virtual inline int precedence() const override { return 10; }
    // C11 6.5.12 — the usual arithmetic conversions, TokenBand's rule.
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// logical or operator ||
class TokenLor: public TokenMultiOp
{
public:
    TokenLor() : TokenMultiOp("||") {}
    virtual TokenID id() const override { return TokenID::tkLor; }
    virtual TokenBase *clone() override { return new TokenLor(); }
    virtual inline int precedence() const override { return 12; }
    virtual bool yields_truth_value() const override { return true; }
};

// bitwise xor operator ^ (exclusive or)
class TokenXor: public TokenOperator
{
public:
    TokenXor() : TokenOperator('^') {}
    virtual TokenID id() const override { return TokenID::tkXor; }
    virtual TokenBase *clone() override { return new TokenXor(); }
    virtual inline int precedence() const override { return 9; }
    // C11 6.5.11 — the usual arithmetic conversions, TokenBand's rule.
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	DataDef *ld = promoted_operand_type(left);
	DataDef *rd = promoted_operand_type(right);
	if ( DataDef *ua = usual_arithmetic_result(ld, rd) ) return ua;
	return TokenOperator::datadef();
    }
};

// ternary operator ? (if)
class TokenTerQ: public TokenOperator
{
public:
    TokenBase *condition;
    TokenBase *true_expr;
    TokenBase *false_expr;
    TokenTerQ() : TokenOperator('?'), condition(NULL), true_expr(NULL), false_expr(NULL) {}
    virtual TokenID id() const override { return TokenID::tkTerQ; }
    virtual TokenBase *clone() override { return new TokenTerQ(); }
    virtual inline int precedence()   const override { return 13; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
    virtual TokenTerQ *as_terq_tok() override { return this; }
};

// ternary operator : (else)
class TokenTerC: public TokenOperator
{
public:
    TokenTerC() : TokenOperator(':') {}
    virtual TokenID id() const override { return TokenID::tkTerC; }
    virtual TokenBase *clone() override { return new TokenTerC(); }
    virtual inline int precedence()   const override { return 13; }
    virtual inline TokenAssoc assoc() const override { return TokenAssoc::taRightToLeft; }
    virtual size_t argc() const override { return 1; }
};

// comparison operator == (equal to)
class TokenEquals: public TokenMultiOp
{
public:
    TokenEquals() : TokenMultiOp("==") {}
    virtual TokenID id() const override { return TokenID::tkEquals; }
    virtual TokenBase *clone() override { return new TokenEquals(); }
    virtual inline int precedence() const override { return 7; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator === (exactly equal to)
class Token3Eq: public TokenMultiOp
{
public:
    Token3Eq() : TokenMultiOp("===") {}
    virtual TokenID id() const override { return TokenID::tk3Eq; }
    virtual TokenBase *clone() override { return new Token3Eq(); }
    virtual inline int precedence() const override { return 7; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator !== (not exactly equal to) — !(===)
class Token3NotEq: public TokenMultiOp
{
public:
    Token3NotEq() : TokenMultiOp("!==") {}
    virtual TokenID id() const override { return TokenID::tk3NotEq; }
    virtual TokenBase *clone() override { return new Token3NotEq(); }
    virtual inline int precedence() const override { return 7; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator != (not equal to)
class TokenNotEq: public TokenMultiOp
{
public:
    TokenNotEq() : TokenMultiOp("!=") {}
    virtual TokenID id() const override { return TokenID::tkNotEq; }
    virtual TokenBase *clone() override { return new TokenNotEq(); }
    virtual inline int precedence() const override { return 7; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator < (less than)
class TokenLT: public TokenOperator
{
public:
    TokenLT() : TokenOperator('<') {}
    virtual TokenID id() const override { return TokenID::tkLT; }
    virtual TokenBase *clone() override { return new TokenLT(); }
    virtual inline int precedence() const override { return 6; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator < (greater than)
class TokenGT: public TokenOperator
{
public:
    TokenGT() : TokenOperator('>') {}
    virtual TokenID id() const override { return TokenID::tkGT; }
    virtual TokenBase *clone() override { return new TokenGT(); }
    virtual inline int precedence() const override { return 6; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator <= (less than or equal to)
class TokenLE: public TokenMultiOp
{
public:
    TokenLE() : TokenMultiOp("<=") {}
    virtual TokenID id() const override { return TokenID::tkLE; }
    virtual TokenBase *clone() override { return new TokenLE(); }
    virtual inline int precedence() const override { return 6; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator <= (greater than or equal to)
class TokenGE: public TokenMultiOp
{
public:
    TokenGE() : TokenMultiOp(">=") {}
    virtual TokenID id() const override { return TokenID::tkGE; }
    virtual TokenBase *clone() override { return new TokenGE(); }
    virtual inline int precedence() const override { return 6; }
    virtual bool yields_truth_value() const override { return true; }
};

// comparison operator <=> (three-way greater than, less than or equal to)
// evaluates to either -1 (<), 0 (=), or 1 (>)
class Token3Way: public TokenMultiOp
{
public:
    Token3Way() : TokenMultiOp("<=>") {}
    virtual TokenID id() const override { return TokenID::tk3Way; }
    virtual TokenBase *clone() override { return new Token3Way(); }
    virtual inline int precedence() const override { return 6; }
};

// bitwise shift left <<
class TokenBSL: public TokenMultiOp
{
    public: TokenBSL() : TokenMultiOp("<<") {}
    virtual TokenID id() const override { return TokenID::tkBSL; }
    virtual TokenBase *clone() override { TokenBSL *to = new TokenBSL(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual inline int precedence() const override { return 5; }
    // C99 6.5.7#3: a shift's type is the PROMOTED LEFT operand's — the
    // right operand never participates (unlike the usual arithmetic
    // conversions; c-testsuite 00200). resolved_type keeps overloaded
    // operator<< (iostreams) authoritative.
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	DataDef *ld = promoted_operand_type(left);
	// a vector's shift is the vector (gcc's vector extension)
	if ( ld && ld->is_simd() )
	    return ld;
	if ( ld && ld->is_integer() && !ld->is_pointer() )
	    if ( DataDef *ua = usual_arithmetic_result(ld, ld) ) return ua;
	return TokenOperator::datadef();
    }
};

// bitwise shift right >>
class TokenBSR: public TokenMultiOp
{
    public: TokenBSR() : TokenMultiOp(">>") {}
    virtual TokenID id() const override { return TokenID::tkBSR; }
    virtual TokenBase *clone() override { TokenBSR *to = new TokenBSR(); to->left = left; to->right = right; to->resolved_type = resolved_type; return to; }
    virtual inline int precedence() const override { return 5; }
    // C99 6.5.7#3 — see TokenBSL (operator>> stays via resolved_type).
    virtual DataDef *datadef() const override
    {
	if ( resolved_type ) return resolved_type;
	DataDef *ld = promoted_operand_type(left);
	// a vector's shift is the vector (gcc's vector extension)
	if ( ld && ld->is_simd() )
	    return ld;
	if ( ld && ld->is_integer() && !ld->is_pointer() )
	    if ( DataDef *ua = usual_arithmetic_result(ld, ld) ) return ua;
	return TokenOperator::datadef();
    }
};

// namespace operator ::
class TokenNS: public TokenMultiOp
{
public:
    TokenNS() : TokenMultiOp("::") {}
    virtual TokenID id() const override { return TokenID::tkNS; }
    virtual TokenBase *clone() override { return new TokenNS(); }
    virtual inline int precedence() const override { return 1; }
};


// dereference struct/class operator ->
class TokenDeRef: public TokenMultiOp
{
public:
    TokenDeRef() : TokenMultiOp("->") {}
    virtual TokenID id() const override { return TokenID::tkDeRef; }
    virtual TokenBase *clone() override { return new TokenDeRef(); }
    virtual inline int precedence() const override { return 1; }
};

// fat-arrow operator => — only meaningful inside a rust::match arm.
// Parsed by TokenMatch::parse(); the lexer just emits the token so the
// match parser can recognize arm boundaries without leaning on local
// `=` / `>` lookahead.
class TokenFatArrow: public TokenMultiOp
{
public:
    TokenFatArrow() : TokenMultiOp("=>") {}
    virtual TokenID id() const override { return TokenID::tkFatArrow; }
    virtual TokenBase *clone() override { return new TokenFatArrow(); }
};

// dot operator . (structure/union/class access)
class TokenDot: public TokenPrimary
{
public:
    TokenDot() : TokenPrimary('.') {}
    virtual TokenID id() const override { return TokenID::tkDot; }
    virtual TokenBase *clone() override { return new TokenDot(); }
    virtual inline int precedence() const override { return 1; }
};

// command operator , (perform first, second and return second result)
class TokenComma: public TokenOperator { public: TokenComma()  : TokenOperator(',') {} virtual TokenID id() const override { return TokenID::tkComma; }  virtual TokenBase *clone() override { return new TokenComma(); }
    // Comma operator: evaluate left for side effects, return right's value.
    // Used by parseExprStmt to chain `e1, e2, e3;` expression-statements
    // (notably brace-less while/for bodies like `++p, ++i;`). Without this,
    // parseExpression stopped at the first comma and the rest was dropped.
    virtual DataDef *datadef() const override {
	// Child queried ONCE (the TokenAdd exponential-recursion rule).
	DataDef *rd = operand_value_type(right);
	if ( rd )
	    return rd;
	return TokenOperator::datadef();
    }
};


// symbols

class TokenSymbol: public TokenBase
{
public:
    TokenSymbol() : TokenBase() {}
    TokenSymbol(int v) : TokenBase(v) {}
    virtual TokenBase *clone() override { return new TokenSymbol(_token); }
    virtual TokenType type() const override { return TokenType::ttSymbol; }
};

// symbol tokens
class TokenHash:  public TokenSymbol   { public: TokenHash()   :   TokenSymbol('#') {} virtual TokenID id() const override { return TokenID::tkHash; }   virtual TokenBase *clone() override { return new TokenHash(); } };
class TokenBslsh: public TokenSymbol   { public: TokenBslsh()  :  TokenSymbol('\\') {} virtual TokenID id() const override { return TokenID::tkBslsh; }  virtual TokenBase *clone() override { return new TokenBslsh(); } };
class TokenOpBrc: public TokenSymbol   { public: TokenOpBrc()  :   TokenSymbol('{') {} virtual TokenID id() const override { return TokenID::tkOpBrc; }  virtual TokenBase *clone() override { return new TokenOpBrc(); } };
class TokenClBrc: public TokenSymbol   { public: TokenClBrc()  :   TokenSymbol('}') {} virtual TokenID id() const override { return TokenID::tkClBrc; }  virtual TokenBase *clone() override { return new TokenClBrc(); } };
class TokenOpBrk: public TokenPrimary  { public: TokenOpBrk()  :  TokenPrimary('(') {} virtual TokenID id() const override { return TokenID::tkOpBrk; }  virtual TokenBase *clone() override { return new TokenOpBrk(); } };
class TokenClBrk: public TokenPrimary  { public: TokenClBrk()  :  TokenPrimary(')') {} virtual TokenID id() const override { return TokenID::tkClBrk; }  virtual TokenBase *clone() override { return new TokenClBrk(); } };
class TokenOpSqr: public TokenPrimary  { public: TokenOpSqr()  :  TokenPrimary('[') {} virtual TokenID id() const override { return TokenID::tkOpSqr; }  virtual TokenBase *clone() override { return new TokenOpSqr(); } };
class TokenClSqr: public TokenPrimary  { public: TokenClSqr()  :  TokenPrimary(']') {} virtual TokenID id() const override { return TokenID::tkClSqr; }  virtual TokenBase *clone() override { return new TokenClSqr(); } };
class TokenSemi:  public TokenSymbol   { public: TokenSemi()   :   TokenSymbol(';') {} virtual TokenID id() const override { return TokenID::tkSemi; }   virtual TokenBase *clone() override { return new TokenSemi(); } };
class TokenColEq: public TokenSymbol   { public: TokenColEq()  :  TokenSymbol(':') {} virtual TokenID id() const override { return TokenID::tkColEq; } virtual TokenBase *clone() override { return new TokenColEq(); } };
class TokenQuote: public TokenSymbol   { public: TokenQuote()  :   TokenSymbol('"') {} virtual TokenID id() const override { return TokenID::tkQuote; }  virtual TokenBase *clone() override { return new TokenQuote(); } };
class TokenApost: public TokenSymbol   { public: TokenApost()  :  TokenSymbol('\'') {} virtual TokenID id() const override { return TokenID::tkApost; }  virtual TokenBase *clone() override { return new TokenApost(); } };


// base numerics

class TokenChar: public TokenBase
{
public:
    TokenChar() : TokenBase()       { _datatype = &ddCHAR; }
    TokenChar(int v) : TokenBase(v) { _datatype = &ddCHAR; }
    virtual TokenBase *clone() override { return new TokenChar(_token); }
    virtual int64_t ival() const override { return _token; }
    // const override — the missing const made this HIDE (not override) the
    // base's is_constant(), so virtual dispatch reported char literals
    // non-constant (clang -Woverloaded-virtual caught it).
    virtual bool is_constant() const override { return true; }
    virtual TokenType type() const override { return TokenType::ttChar; }
    virtual TokenID   id()   const override { return TokenID::tkChar; }
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
    // The parser's stand-in for a call it could not resolve inside a
    // dependent (tsubst-pattern) or class-pattern CAPTURE parse — a `0`
    // typed int64 where a call belongs. Anything that would bake this
    // token's TYPE into a retained pattern (a `decltype(...)` alias in a
    // class template body) must treat it as poison, never as a constant.
    bool dependent_call_placeholder = false;
    TokenInt() : TokenBase()            { _datatype = &ddINT; }
    TokenInt(int64_t v) : TokenBase(v) { _datatype = &ddINT; }
    TokenInt(int64_t v, const std::string &src) : TokenBase(v), source_text(src) { _datatype = &ddINT; }
    virtual int64_t ival() const override        { return _token; }
    // Semantically wide only when the constant's own TYPE is 16 bytes
    // (ddINT128/ddUINT128, set by the parser's fold); see TokenBase::wival.
    virtual madc_wide_int wival() const override
    {
	if ( wide_handle && _active_valpool && _datatype && _datatype->size == 16 )
	    return (madc_wide_int)(((madc_wide_uint)_active_valpool->hi64(wide_handle) << 64)
				   | _active_valpool->lo64(wide_handle));
	return _token;
    }
    virtual double dval() const override    { return (double)_token; }
    virtual TokenType type() const override { return TokenType::ttInteger; }
    virtual TokenID   id()   const override { return TokenID::tkInt; }
    virtual TokenBase *clone() override     { auto *c = new TokenInt(_token); c->source_text = source_text; c->_datatype = _datatype; c->wide_handle = wide_handle; c->dependent_call_placeholder = dependent_call_placeholder; return c; }
    virtual bool is_constant() const override { return true; }
    virtual void setDataType(DataDef *d) override { if (d && (d->is_integer() || d->is_complex())) _datatype = d; }
    virtual TokenInt *as_int_tok() override { return this; }
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
    virtual TokenBase *clone() override { return new TokenNullptr(); }
    virtual void setDataType(DataDef *d)
 override    {
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
    virtual TokenID id() const override { return TokenID::tkDynamicCast; }
    virtual TokenBase *clone() override { return new TokenDynamicCast(*this); }
    virtual TokenDynamicCast *as_dyncast_tok() override { return this; }
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
    virtual TokenID id() const override { return TokenID::tkTypeid; }
    virtual TokenBase *clone() override { return new TokenTypeid(*this); }
    virtual TokenTypeid *as_typeid_tok() override { return this; }
};

class TokenTypeQuery: public TokenBase
{
public:
    DataDef *query_type;
    bool want_alignof;
    bool use_cached_runtime_size;
    // sizeof of a VLA-typed operand EVALUATES the operand (C11 6.5.3.4p2):
    // the subscript index expressions of a deferred row sizeof, emitted
    // (values discarded) ahead of the runtime size computation.
    std::vector<TokenBase *> operand_side_effects;

    TokenTypeQuery(DataDef *dd = NULL, bool want_align = false,
		   bool use_cached_size = true)
	: TokenBase(), query_type(dd), want_alignof(want_align),
	  use_cached_runtime_size(use_cached_size)
    {
	_datatype = &ddUINT64;
    }
    virtual TokenBase *clone()
 override    {
	TokenTypeQuery *c = new TokenTypeQuery(query_type, want_alignof,
					       use_cached_runtime_size);
	c->operand_side_effects = operand_side_effects;
	return c;
    }
    virtual TokenID id() const override { return TokenID::tkInt; }
    virtual TokenTypeQuery *as_typequery_tok() override { return this; }
};

class TokenReal: public TokenBase
{
protected:
    // Widest real madc has, so an L-suffixed literal keeps every bit it was
    // written with. Storing a double here truncated `1.0L` at lex time, before
    // any type could matter, and no later stage could recover the lost bits.
    long double _val;
public:
    std::string source_text;   // original literal text (suffixes like i, iF, f, L)
    TokenReal() : TokenBase()              { _val = 0; _datatype = &ddDOUBLE; }
    TokenReal(long double v) : TokenBase() { _val = v; _datatype = &ddDOUBLE; }
    virtual int64_t ival() const override      { return (int64_t)_val; }
    virtual double dval() const override       { return (double)_val; }
    // Full stored precision — dval() narrows, so a long-double literal must be
    // read through this on the paths that care (CIR emission, constant folding).
    long double ldval() const                  { return _val;      }
    virtual TokenType type() const override    { return TokenType::ttReal; }
    virtual TokenID   id()   const override    { return TokenID::tkReal;   }
    virtual TokenBase *clone() override        { auto *c = new TokenReal(_val); c->source_text = source_text; c->_datatype = _datatype; return c; }
    virtual bool is_constant() const override { return true; }
    virtual bool is_real()     const override { return true; }
    virtual void setDataType(DataDef *d) override { if (d && (d->is_real() || d->is_complex())) _datatype = d; }
    virtual TokenReal *as_real_tok() override { return this; }
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
    virtual TokenType type() const override { return TokenType::ttIdentifier; }
    virtual TokenID   id()   const override { return TokenID::tkIdent; }
    virtual TokenBase *clone() override     { TokenIdent *t = new TokenIdent(); t->rec.spelling_id = rec.spelling_id; return t; }
    virtual void setDataType(DataDef *d) override { if (d) _datatype = d; }
    virtual TokenIdent *as_ident_tok() override { return this; }
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
    // Source extent of each concatenated literal PIECE (line, start column
    // of the opening quote/prefix, RAW source length including the quotes),
    // recorded at lex. The highlight-span classifier's anchor: the cooked
    // `str` cannot recover source geometry — escapes shrink it, and C
    // adjacent-literal concatenation merges several source lines into one
    // token (one 85-byte span painted from line 30 col 0 was madcide's
    // embed_hello.c mis-highlight). Empty for tokens with no source
    // (pack-image materialization, synthesized literals) — consumers fall
    // back to the spelling-derived extent.
    struct SrcPiece { int32_t line, col, len; };
    std::vector<SrcPiece> src_pieces;
    TokenStr() : wide(false) {}
    TokenStr(const char *k, bool w = false) : TokenIdent(k), str(k ? k : ""), wide(w) {}
    TokenStr(std::string k, bool w = false) : TokenIdent(k), str(k), wide(w) {}
    virtual const char *spelling() const override { return str.c_str(); }
    virtual size_t spelling_len() const override { return str.size(); }
    virtual int64_t ival() const override   { return atol(str.c_str()); }
    virtual bool is_constant() const override { return true; }
    virtual TokenType type() const override { return TokenType::ttString; }
    virtual TokenID   id()   const override { return TokenID::tkStr; }
    virtual TokenBase *clone() override     { TokenStr *t = new TokenStr(str, wide); t->src_pieces = src_pieces; return t; }
    virtual TokenStr *as_str_tok() override { return this; }
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
    virtual TokenType type() const override { return TokenType::ttComment; }
    virtual TokenID   id()   const override { return TokenID::tkREM; }
    virtual TokenBase *clone() override     { return new TokenREM(str); }
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
    virtual TokenType type() const override { return TokenType::ttKeyword; }
//  virtual TokenBase *clone(){ return new TokenKeyword(str); }
    virtual TokenBase *clone() override{ return this; }
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
    virtual TokenID id() const override { return TokenID::tkCPPKEYWORD; }
    virtual TokenBase *clone() override { return new TokenCppKeyword(str); }
    // Ignored declaration-specifiers (constexpr/consteval/constinit) consume
    // themselves and continue parsing the declaration they qualify; any other
    // reserved keyword reaching here is an expression leader.
    virtual TokenBase *parse(Program &) override;
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

class TokenELSE:     public TokenKeyword { public: TokenELSE()     : TokenKeyword("else") {}     virtual TokenID id() const override { return TokenID::tkELSE;     } virtual TokenBase *clone() override { return (TokenBase*)new TokenELSE();    } };
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
    virtual TokenID id() const override { return TokenID::tkGOTO; }
    virtual TokenBase *clone() override { return (TokenBase*)new TokenGOTO(); }
    virtual TokenBase *parse(Program &) override;
    virtual TokenGOTO *as_goto_tok() override { return this; }
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
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual TokenBase *clone()
 override    {
	TokenLabel *t = new TokenLabel(name);
	if ( labeled )
	    t->labeled = labeled->clone();
	return t;
    }
    virtual TokenLabel *as_label_tok() override { return this; }
};

// madc-dialect `go <call-expr>;` (MT-1): spawn the call as a cooperative
// task. CONTEXTUAL statement head under STD_MADC (never keyword_map-reserved
// — see tkGO in the TokenID enum), built by Program::parse_go_statement via
// the UFCS error-shape rule. Carries the parsed, resolved call; the CIR
// builder lowers it to a per-site thunk + __madc_go (src/rt/rt_task.c).
class TokenGO: public TokenBase
{
public:
    TokenBase *call;
    TokenGO() : call(NULL) {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual TokenID id() const override { return TokenID::tkGO; }
    virtual TokenBase *clone()
 override    {
	TokenGO *t = new TokenGO();
	if ( call )
	    t->call = call->clone();
	return t;
    }
};

// madc-dialect `yield;` / `yield();` (MT-1): reschedule the current
// cooperative task. Contextual twin of TokenGO (same gating and error-shape
// rule); lowers to a __madc_yield() call.
class TokenYIELD: public TokenBase
{
public:
    TokenYIELD() {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual TokenID id() const override { return TokenID::tkYIELD; }
    virtual TokenBase *clone() override { return new TokenYIELD(); }
};

// madc-dialect `scope { ... }` (MT-5): a structured-concurrency block —
// `go` inside attaches to it; the block's end joins every member and
// rethrows the first member error (madc::scope_end's contract). Contextual
// statement head under STD_MADC (same gating and error-shape rule as
// TokenGO); the CIR builder lowers it to the __madc_scope_block_enter /
// __madc_scope_block_exit pair (src/rt/rt_task.c). return / break /
// continue / goto that would exit the block are refused at parse time.
class TokenSCOPE: public TokenBase
{
public:
    TokenBase *body;                       // the compound block
    TokenSCOPE() : body(NULL) {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual TokenID id() const override { return TokenID::tkSCOPE; }
    virtual TokenBase *clone() override
    {
	TokenSCOPE *t = new TokenSCOPE();
	if ( body )
	    t->body = body->clone();
	return t;
    }
};

// madc-dialect `await <chan-expr>` (MT-5): receive from a value channel —
// Go's `<-ch` (blocks; closed-and-drained yields the zero value). Claimed
// in the expression ladder's undeclared-identifier position (error-shape
// rule) and consumed at STATEMENT level in slice 1: bare `await ch;`
// (target NULL — receive and discard) and `v = await ch;` (parseExprStmt
// extracts the value-typed target). The CIR builder lowers it to
// __madc_chan_await(&target|NULL, handle) — the extern-C machinery seat
// over THE ONE recv implementation — and refuses any other position loud.
class TokenAWAIT: public TokenBase
{
public:
    TokenBase *chan;                       // the channel-handle expression
    TokenBase *target;                     // resolved value-typed lvalue
					   // (NULL = receive and discard)
    TokenAWAIT() : chan(NULL), target(NULL) {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual TokenID id() const override { return TokenID::tkAWAIT; }
    virtual TokenBase *clone() override
    {
	TokenAWAIT *t = new TokenAWAIT();
	if ( chan )
	    t->chan = chan->clone();
	if ( target )
	    t->target = target->clone();
	return t;
    }
};
class TokenCASE: public TokenKeyword
{
public:
    TokenBase *value;                          // case constant expression
    TokenBase *range_high;                     // GNU case range: case LOW ... HIGH
    std::vector<TokenBase *> statements;       // statements until next case/default/}
    // Non-empty for a label NESTED inside a statement of the switch body
    // (Duff's device: `case 7:` inside a do-while). The statement stays in
    // its enclosing structure with a TokenLabel of this name in place, and
    // the switch dispatch emits `case V: goto <name>;` — restructuring the
    // body into per-case buckets would gut the enclosing loop/if.
    std::string in_place_label;
    TokenCASE() : TokenKeyword("case"), value(NULL), range_high(NULL) {}
    virtual TokenID id() const override { return TokenID::tkCASE; }
    virtual TokenBase *clone() override { return new TokenCASE(); }
    virtual TokenBase *parse(Program &) override;
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
    virtual TokenID id() const override { return TokenID::tkTRY; }
    virtual TokenBase *clone() override { return new TokenTRY(); }
    virtual TokenBase *parse(Program &) override;
    virtual TokenTRY *as_try_tok() override { return this; }
};
class TokenCATCH:    public TokenKeyword { public: TokenCATCH()    : TokenKeyword("catch") {}    virtual TokenID id() const override { return TokenID::tkCATCH;    } virtual TokenBase *clone() override { return (TokenBase*)new TokenCATCH();   } };
// throw expr — throws an exception
class TokenTHROW: public TokenKeyword
{
public:
    TokenBase *throw_expr;  // expression to throw (NULL for rethrow)
    TokenTHROW() : TokenKeyword("throw") { throw_expr = NULL; }
    virtual TokenID id() const override { return TokenID::tkTHROW; }
    virtual TokenBase *clone() override { return new TokenTHROW(); }
    virtual TokenBase *parse(Program &) override;
    virtual TokenTHROW *as_throw_tok() override { return this; }
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
    virtual TokenID id() const override { return TokenID::tkSWITCH; }
    virtual TokenBase *clone() override { return new TokenSWITCH(); }
    virtual TokenBase *parse(Program &) override;
    virtual TokenSWITCH *as_switch_tok() override { return this; }
};
class TokenCLASS: public TokenKeyword
{
public:
    TokenCLASS() : TokenKeyword("class") {}
    virtual TokenID id() const override { return TokenID::tkCLASS; }
    virtual TokenBase *clone() override { return (TokenBase*)new TokenCLASS(); }
    virtual TokenBase *parse(Program &) override;
};
class TokenDEFAULT:  public TokenKeyword { public: TokenDEFAULT()  : TokenKeyword("default") {}  virtual TokenID id() const override { return TokenID::tkDEFAULT;  } virtual TokenBase *clone() override { return (TokenBase*)new TokenDEFAULT(); } };
class TokenTYPEDEF: public TokenKeyword
{
public:
    TokenTYPEDEF() : TokenKeyword("typedef") {}
    virtual TokenID id() const override { return TokenID::tkTYPEDEF; }
    virtual TokenBase *clone() override { return new TokenTYPEDEF(); }
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenNAMESPACE:public TokenKeyword { public: TokenNAMESPACE() : TokenKeyword("namespace") {} virtual TokenID id() const override { return TokenID::tkNAMESPACE; } virtual TokenBase *clone() override { return (TokenBase*)new TokenNAMESPACE(); } virtual TokenBase *parse(Program &) override; };

class TokenUSING: public TokenKeyword
{
public:
    TokenUSING() : TokenKeyword("using") {}
    virtual TokenID id() const override { return TokenID::tkUSING; }
    virtual TokenBase *clone() override { return (TokenBase*)new TokenUSING(); }
    virtual TokenBase *parse(Program &) override;
};

// C++ `friend` declaration specifier. Only valid leading a member declaration
// inside a class/struct body (the struct/class member parsers intercept it on
// the tkFRIEND token); a standalone parse is a misplaced-friend error.
class TokenFRIEND: public TokenKeyword
{
public:
    TokenFRIEND() : TokenKeyword("friend") {}
    virtual TokenID id() const override { return TokenID::tkFRIEND; }
    virtual TokenBase *clone() override { return (TokenBase*)new TokenFRIEND(); }
    virtual TokenBase *parse(Program &) override;
};

class TokenPREFER: public TokenKeyword
{
public:
    TokenPREFER() : TokenKeyword("prefer") {}
    virtual TokenID id() const override { return TokenID::tkPREFER; }
    virtual TokenBase *clone() override { return (TokenBase*)new TokenPREFER(); }
    virtual TokenBase *parse(Program &) override;
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
    virtual TokenID id() const override { return TokenID::tkMATCH; }
    virtual TokenBase *clone() override { return new TokenMatch(); }
    virtual TokenBase *parse(Program &) override;
    virtual TokenMatch *as_match_tok() override { return this; }
};

// defer keyword: register a statement to run at scope exit (LIFO)
class TokenDEFER: public TokenKeyword
{
public:
    TokenDEFER() : TokenKeyword("defer") {}
    virtual TokenID id() const override { return TokenID::tkDEFER; }
    virtual TokenBase *clone() override { return new TokenDEFER(); }
    virtual TokenBase *parse(Program &) override;
};

// STL container keywords are gone: std::map/set/list are header-defined madc
// templates (include/madc/map, include/madc/set), instantiated through the
// class model — not lexer keywords.

// `template<typename T> class Name { ... }` — capture the definition for
// Borland-model instantiation. parse() captures (typeparams, class-body token
// range) into Program::template_map without parsing the body (T is unbound);
// `Name<ConcreteType>` later clones+substitutes+re-parses it as a concrete class.
// See docs/plans/2026-05-30-template-instantiation.md.
class TokenTEMPLATE: public TokenKeyword { public: TokenTEMPLATE() : TokenKeyword("template") {} virtual TokenID id() const override { return TokenID::tkTEMPLATE; } virtual TokenBase *clone() override { return new TokenTEMPLATE(); } virtual TokenBase *parse(Program &) override; };

// new / delete — heap allocation with constructor/destructor calls
class TokenNEW: public TokenKeyword
{
public:
    DataDefCLASS *alloc_class;
    std::vector<TokenBase *> ctor_args;
    bool braced = false; // list-initialization selects braced constructor overloads
    // Placement new: `new (placement) Type(args)` constructs at the given
    // address instead of allocating. `placement` is the address expression
    // (NULL for ordinary `new`); `alloc_type` is the constructed type when it
    // is not a class (string / scalar), with alloc_class still used for classes.
    TokenBase *placement;
    DataDef *alloc_type;
    TokenBase *array_size;	// `new T[n]` — the element count expr (NULL for scalar new)
    // The expression's TYPE ([expr.new]/1: a prvalue of type `T *`), set by
    // parse() for both the scalar and the array form. A new-expression is the
    // keyword token itself, so without this datadef() answered the keyword
    // default and `auto c = new T(...)` deduced `char`.
    DataDef *result_type;
    TokenNEW() : TokenKeyword("new") { alloc_class = NULL; placement = NULL; alloc_type = NULL; array_size = NULL; result_type = NULL; }
    virtual TokenID id() const override { return TokenID::tkNEW; }
    virtual TokenBase *clone() override { return new TokenNEW(); }
    virtual TokenBase *parse(Program &) override;
    virtual DataDef *datadef() const override
    { return result_type ? result_type : (_datatype ? _datatype : &ddVOID); }
    virtual TokenNEW *as_new_tok() override { return this; }
};
class TokenDELETE: public TokenKeyword
{
public:
    TokenBase *expr;
    DataDefCLASS *del_class;
    bool is_array;	// `delete[]` (array delete) vs scalar `delete`
    TokenDELETE() : TokenKeyword("delete") { expr = NULL; del_class = NULL; is_array = false; }
    virtual TokenID id() const override { return TokenID::tkDELETE; }
    virtual TokenBase *clone() override { return new TokenDELETE(); }
    virtual TokenBase *parse(Program &) override;
    virtual TokenDELETE *as_delete_tok() override { return this; }
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
    // Which spelling opened the argument list survives to construction
    // ([dcl.init.list]/3-4): the braced form `T{...}` list-initializes (an
    // initializer-list ctor takes the WHOLE list as one argument; a class
    // that IS std::initializer_list builds from the backing array directly),
    // the paren form `T(...)` never does. TokenDecl::ctor_args_braced is the
    // declaration path's twin of this flag.
    bool braced = false;
    // KEYED carrier-literal elements (`{"a": 1}` in expression position,
    // owner 2026-08-31): PARALLEL to ctor_args, NULL = positional — the
    // TokenDecl::ctor_arg_keys convention. Populated only for a braced
    // list whose obj_class is the carrier (ddARRAY).
    std::vector<TokenBase *> ctor_arg_keys;
    TokenObjTemp(DataDefCLASS *c) : TokenBase() { obj_class = c; _datatype = (DataDef *)c; }
    virtual TokenID id() const override { return TokenID::tkObjTemp; }
    virtual DataDef *datadef() const override { return (DataDef *)obj_class; }
    virtual TokenBase *clone() override { return new TokenObjTemp(*this); }
    virtual TokenObjTemp *as_objtemp_tok() override { return this; }
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
    virtual TokenID id() const override { return TokenID::tkExplicitDtor; }
    virtual DataDef *datadef() const override { return &ddVOID; }
    virtual TokenBase *clone() override { return new TokenExplicitDtor(*this); }
    virtual TokenExplicitDtor *as_explicit_dtor_tok() override { return this; }
};

class TokenSTRUCT: public TokenKeyword
{
public:
    TokenSTRUCT() : TokenKeyword("struct") {}
    virtual TokenID id() const override { return TokenID::tkSTRUCT; }
    virtual TokenBase *clone() override { return new TokenSTRUCT(); }
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenUNION: public TokenSTRUCT
{
public:
    TokenUNION() : TokenSTRUCT() { str = "union"; }
    virtual TokenID id() const override { return TokenID::tkUNION; }
    virtual TokenBase *clone() override { return new TokenUNION(); }
};

// register keyword: declares a variable that lives only in a virtual register
// (never written to memory), for maximum performance in hot loops
class TokenREGISTER: public TokenKeyword
{
public:
    TokenREGISTER() : TokenKeyword("register") {}
    virtual TokenID id() const override { return TokenID::tkREGISTER; }
    virtual TokenBase *clone() override { return new TokenREGISTER(); }
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenSTATIC: public TokenKeyword
{
public:
    TokenSTATIC() : TokenKeyword("static") {}
    virtual TokenID id() const override { return TokenID::tkSTATIC; }
    virtual TokenBase *clone() override { return new TokenSTATIC(); }
    virtual TokenBase *parse(Program &pgm) override;
};

// const and extern are consumed and ignored for C compatibility
class TokenCONST: public TokenKeyword
{
public:
    TokenCONST() : TokenKeyword("const") {}
    virtual TokenID id() const override { return TokenID::tkCONST; }
    virtual TokenBase *clone() override { return new TokenCONST(); }
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenEXTERN: public TokenKeyword
{
public:
    TokenEXTERN() : TokenKeyword("extern") {}
    virtual TokenID id() const override { return TokenID::tkEXTERN; }
    virtual TokenBase *clone() override { return new TokenEXTERN(); }
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenRESTRICT: public TokenKeyword
{
public:
    TokenRESTRICT() : TokenKeyword("restrict") {}
    virtual TokenID id() const override { return TokenID::tkRESTRICT; }
    virtual TokenBase *clone() override { return new TokenRESTRICT(); }
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenVOLATILE: public TokenKeyword
{
public:
    TokenVOLATILE() : TokenKeyword("volatile") {}
    virtual TokenID id() const override { return TokenID::tkVOLATILE; }
    virtual TokenBase *clone() override { return new TokenVOLATILE(); }
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenENUM: public TokenKeyword
{
public:
    TokenENUM() : TokenKeyword("enum") {}
    virtual TokenID id() const override { return TokenID::tkENUM; }
    virtual TokenBase *clone() override { return new TokenENUM(); }
    virtual TokenBase *parse(Program &pgm) override;
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
    virtual TokenType type() const override { return TokenType::ttBase; }
};

class TokenBREAK: public TokenKeyword
{
public:
    TokenBREAK() : TokenKeyword("break") {}
    virtual TokenID id() const override { return TokenID::tkBREAK; }
    virtual TokenBase *clone() override { return new TokenBREAK(); }
    // Out-of-line (parser.cpp): the MT-5 scope-block crossing check.
    virtual TokenBase *parse(Program &pgm) override;
};

class TokenCONT: public TokenKeyword
{
public:
    TokenCONT() : TokenKeyword("continue") {}
    virtual TokenID id() const override { return TokenID::tkCONT;  }
    virtual TokenBase *clone() override { return new TokenCONT();  }
    // Out-of-line (parser.cpp): the MT-5 scope-block crossing check.
    virtual TokenBase *parse(Program &pgm) override;
};


class TokenOPEROVER: public TokenKeyword
{
public:
    TokenOPEROVER() : TokenKeyword("operator") {}
    virtual TokenID id() const override { return TokenID::tkOPEROVER; }
    virtual TokenBase *clone() override { return new TokenOPEROVER(); }
    virtual TokenBase *parse(Program &) override;
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
    virtual TokenBase *parse(Program &) override;
    virtual TokenID id() const override { return TokenID::tkIF; }
    virtual TokenBase *clone() override { return new TokenIF(); }
    virtual TokenIF *as_if_tok() override { return this; }
};

class TokenRETURN: public TokenKeyword
{
public:
    TokenBase *returns;
    std::vector<TokenBase *> return_exprs; // multi-return: return a, b;
    TokenRETURN() : TokenKeyword("return") { returns = NULL; }
    virtual TokenBase *parse(Program &) override;
    virtual TokenID id() const override { return TokenID::tkRETURN; }
    virtual TokenBase *clone() override { return new TokenRETURN(); }
    virtual TokenRETURN *as_return_tok() override { return this; }
};

class TokenDO: public TokenKeyword
{
public:
    TokenBase *statement;
    TokenBase *condition;
    TokenDO() : TokenKeyword("do") { statement = condition = NULL; }
    virtual TokenBase *parse(Program &) override;
    virtual TokenID id() const override { return TokenID::tkDO; }
    virtual TokenBase *clone() override { return new TokenDO(); }
    virtual TokenDO *as_do_tok() override { return this; }
};

class TokenWHILE: public TokenKeyword
{
public:
    TokenBase *condition;
    TokenBase *statement;
    TokenWHILE() : TokenKeyword("while") { condition = statement = NULL; }
    virtual TokenBase *parse(Program &) override;
    virtual TokenID id() const override { return TokenID::tkWHILE; }
    virtual TokenBase *clone() override { return new TokenWHILE(); }
    virtual TokenWHILE *as_while_tok() override { return this; }
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
    virtual TokenBase *parse(Program &) override;
    virtual TokenID id() const override { return TokenID::tkFOR; }
    virtual TokenBase *clone() override { return new TokenFOR(); }
    virtual TokenFOR *as_for_tok() override { return this; }
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
    virtual TokenID id() const override { return TokenID::tkFOR; }
    virtual TokenBase *clone() override { return new TokenFOREACH(); }
    virtual TokenFOREACH *as_foreach_tok() override { return this; }
};


#endif // __TOKENS_H
