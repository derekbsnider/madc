#ifndef __TOKENDATA_H
//////////////////////////////////////////////////////////////////////////
//									//
// Data Type Tokens							//
//									//
//////////////////////////////////////////////////////////////////////////
#define __TOKENDATA_H 1

class TokenDataType: public TokenIdent
{
public:
    DataDef &definition;
    TokenDataType(const char *k, DataDef &d) : TokenIdent(k), definition(d) {}
    virtual TokenType type() const { return TokenType::ttDataType; }
    virtual TokenBase *clone() { return new TokenDataType(str.c_str(), definition); }
};

// token definitions of integral data types
class TokenVOID:      public TokenDataType { public: TokenVOID() :  TokenDataType("void", ddVOID) {} };
class TokenBOOL:      public TokenDataType { public: TokenBOOL() :  TokenDataType("bool", ddBOOL) {} };
class TokenCHAR:      public TokenDataType { public: TokenCHAR() :  TokenDataType("char", ddCHAR) {} };
class TokenINT:       public TokenDataType { public: TokenINT()  :  TokenDataType("int", ddINT) {} };
class TokenINT8:      public TokenDataType { public: TokenINT8() :  TokenDataType("int8_t", ddINT8) {} };
class TokenINT16:     public TokenDataType { public: TokenINT16():  TokenDataType("int16_t", ddINT16) {} };
class TokenINT24:     public TokenDataType { public: TokenINT24():  TokenDataType("int24_t", ddINT24) {} };
class TokenINT32:     public TokenDataType { public: TokenINT32():  TokenDataType("int32_t", ddINT32) {} };
class TokenINT64:     public TokenDataType { public: TokenINT64():  TokenDataType("int64_t", ddINT64) {} };
class TokenUINT8:     public TokenDataType { public: TokenUINT8() : TokenDataType("uint8_t", ddUINT8) {} };
class TokenUINT16:    public TokenDataType { public: TokenUINT16(): TokenDataType("uint16_t", ddUINT16) {} };
class TokenUINT24:    public TokenDataType { public: TokenUINT24(): TokenDataType("uint24_t", ddUINT24) {} };
class TokenUINT32:    public TokenDataType { public: TokenUINT32(): TokenDataType("uint32_t", ddUINT32) {} };
class TokenUINT64:    public TokenDataType { public: TokenUINT64(): TokenDataType("uint64_t", ddUINT64) {} };
class TokenFLOAT:     public TokenDataType { public: TokenFLOAT() : TokenDataType("float", ddFLOAT) {} };
class TokenDOUBLE:    public TokenDataType { public: TokenDOUBLE(): TokenDataType("double", ddDOUBLE) {} };

// char *
class TokenLPSTR:     public TokenDataType { public: TokenLPSTR():  TokenDataType("LPSTR", ddLPSTR) {} };

// some basic c++ types
class TokenSTRING:    public TokenDataType { public: TokenSTRING(): TokenDataType("string", ddSTRING) {} };
class TokenOSTREAM:   public TokenDataType { public: TokenOSTREAM():TokenDataType("ostream", ddOSTREAM) {} };
class TokenSSTREAM:   public TokenDataType { public: TokenSSTREAM():TokenDataType("stringstream", ddSSTREAM) {} };


// Variable "container" class to keep track of everything about a variable,
// primary only used during parsing/compiling
class Variable
{
public:
    std::string name;
    DataDef *type;
    void *data;
    uint32_t count;
    uint16_t flags;
    Variable() { type = &ddINT; data = NULL; flags = 0; count = 0; }
    Variable(std::string n, DataDef &d, uint32_t c = 1, void *init=NULL, bool alloc=true);
   ~Variable();
    inline void modified() { flags |= vfMODIFIED; DBG(std::cout << "Variable::modified(" << name << ')' << std::endl); }
    inline void makeconstant() { flags |= vfCONSTANT; }
    inline bool is_global()   { if ( (flags & vfLOCAL) && !(flags &vfSTATIC) ) return false; return true; }
    inline bool is_constant() { if ( (flags & vfCONSTANT) ) return true; return false; }
    bool set(int c)
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   *((char *)data) = c;
	else if (type == &ddBOOL)   *((bool *)data) = c;
	else if (type == &ddINT)    *((int *)data) = c;
	else if (type == &ddINT8)   *((int8_t *)data) = c;
	else if (type == &ddINT16)  *((int16_t *)data) = c;
	else if (type == &ddINT24)  *((int16_t *)data) = c;
	else if (type == &ddINT32)  *((int32_t *)data) = c;
	else if (type == &ddUINT8)  *((uint8_t *)data) = c;
	else if (type == &ddUINT16) *((uint16_t *)data) = c;
	else if (type == &ddUINT24) *((uint16_t *)data) = c;
	else if (type == &ddUINT32) *((uint32_t *)data) = c;
	else if (type == &ddFLOAT)  *((float *)data) = c;
	else if (type == &ddDOUBLE) *((double *)data) = c;
	else 	     { return false; }
	return true;
    }
    template<typename T> int cmp(T c)
    {
	if ( !data ) { return 0; }
	if (type == &ddCHAR)   return *((char *)data) == c;
	if (type == &ddBOOL)   return *((bool *)data) == c;
	if (type == &ddINT)    return *((int *)data) == c;
	if (type == &ddINT8)   return *((int8_t *)data) == c;
	if (type == &ddINT16)  return *((int16_t *)data) == c;
	if (type == &ddINT24)  return *((int16_t *)data) == c;
	if (type == &ddINT32)  return *((int32_t *)data) == c;
	if (type == &ddUINT8)  return *((uint8_t *)data) == c;
	if (type == &ddUINT16) return *((uint16_t *)data) == c;
	if (type == &ddUINT24) return *((uint16_t *)data) == c;
	if (type == &ddUINT32) return *((uint32_t *)data) == c;
	if (type == &ddFLOAT)  return *((float *)data) == c;
	if (type == &ddDOUBLE) return *((double *)data) == c;
	return 0;
    }
    int cmp(std::string &s)
    {
	if (type == &ddSTRING) return ((std::string *)data)->compare(s);
	return 0;
    }
    bool dec()
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   --*((char *)data);
	else if (type == &ddINT)    --*((int *)data);
	else if (type == &ddINT8)   --*((int8_t *)data);
	else if (type == &ddINT16)  --*((int16_t *)data);
	else if (type == &ddINT24)  --*((int16_t *)data);
	else if (type == &ddINT32)  --*((int32_t *)data);
	else if (type == &ddUINT8)  --*((uint8_t *)data);
	else if (type == &ddUINT16) --*((uint16_t *)data);
	else if (type == &ddUINT24) --*((uint16_t *)data);
	else if (type == &ddUINT32) --*((uint32_t *)data);
	else if (type == &ddFLOAT)  --*((float *)data);
	else if (type == &ddDOUBLE) --*((double *)data);
	return true;
    }
    bool inc()
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   ++*((char *)data);
	else if (type == &ddINT)    ++*((int *)data);
	else if (type == &ddINT8)   ++*((int8_t *)data);
	else if (type == &ddINT16)  ++*((int16_t *)data);
	else if (type == &ddINT24)  ++*((int16_t *)data);
	else if (type == &ddINT32)  ++*((int32_t *)data);
	else if (type == &ddUINT8)  ++*((uint8_t *)data);
	else if (type == &ddUINT16) ++*((uint16_t *)data);
	else if (type == &ddUINT24) ++*((uint16_t *)data);
	else if (type == &ddUINT32) ++*((uint32_t *)data);
	else if (type == &ddFLOAT)  ++*((float *)data);
	else if (type == &ddDOUBLE) ++*((double *)data);
	return true;
    }
    template<typename T> T get()
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   return *((char *)data);
	else if (type == &ddINT)    return *((int *)data);
	else if (type == &ddINT8)   return *((int8_t *)data);
	else if (type == &ddINT16)  return *((int16_t *)data);
	else if (type == &ddINT24)  return *((int16_t *)data);
	else if (type == &ddINT32)  return *((int32_t *)data);
	else if (type == &ddUINT8)  return *((uint8_t *)data);
	else if (type == &ddUINT16) return *((uint16_t *)data);
	else if (type == &ddUINT24) return *((uint16_t *)data);
	else if (type == &ddUINT32) return *((uint32_t *)data);
	else if (type == &ddFLOAT)  return *((float *)data);
	else if (type == &ddDOUBLE) return *((double *)data);
	return true;
    }
};


class TokenVar: public virtual TokenBase
{
public:
    Variable &var;
    TokenVar(Variable &v) : TokenBase(), var(v) { _datatype = v.type; }
    virtual TokenType type() const { return TokenType::ttVariable; }
    virtual int get() const { return var.get<int>(); }
    virtual int val() const { return var.get<int>(); }
    virtual void set(int c) { DBG(std::cout << "TokenVariable: set() calling var.set()" << std::endl); var.set(c); }
    virtual asmjit::x86::Gp &getreg(Program &);
    virtual void putreg(Program &);
    virtual asmjit::x86::Gp &compile(Program &, regdefp_t regdp);
};

#endif // __TOKENDATA_H
