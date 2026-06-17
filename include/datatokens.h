#ifndef __TOKENDATA_H
//////////////////////////////////////////////////////////////////////////
//									//
// Data Type Tokens							//
//									//
//////////////////////////////////////////////////////////////////////////
#define __TOKENDATA_H 1

#include <cstring>

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
class TokenC23BOOL:   public TokenDataType { public: TokenC23BOOL() : TokenDataType("_Bool", ddBOOL) {} };
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
// NOTE: there is deliberately NO TokenSTRING here. `string` is std::string —
// it comes into existence only via `#include <string>` under the `std`
// namespace (std_types["string"], registered in _parser_init), reached as
// `std::string` or via `using namespace std;`. It is NOT a global builtin
// type token, exactly like C++. (Phase A5, stdtypes-as-real-classes.)
class TokenARRAY:     public TokenDataType { public: TokenARRAY():  TokenDataType("array", ddARRAY) {} };

class TokenAUTO:      public TokenDataType { public: TokenAUTO():  TokenDataType("auto", ddAUTO) {} };

// Variable "container" class to keep track of everything about a variable,
// primary only used during parsing/compiling
class Variable
{
public:
    std::string name;
    DataDef *type;
    void *data;
    size_t aot_data_offset;
    size_t aot_cstr_offset;
    uint32_t count;
    uint32_t flags;
    std::string storage_alias_name;
    std::string typedef_name; // if declared via typedef, the source alias (e.g. "EXT_BV")
    // Explicit '*' count written on a function-type-typedef declarator, recorded
    // because the type stays a bare DataDefFPTR (fn-ptr CALL detection keys on it,
    // so the stars can't live in the type). `DO_FUN x` -> 0 (a C function
    // declaration), `DO_FUN *fp` -> 1 (a function-pointer variable). -1 means
    // "not a recorded fn-ptr-base declarator" -> emitter uses its legacy path.
    int fnptr_explicit_stars = -1;
    std::vector<carray_dim_t> dims; // C fixed-size array shape; empty = scalar
    int64_t object_size_hint;
    // C99 variable-length array: when non-NULL, the local was declared as
    // `T name[expr]` with a runtime-valued size. The variable acts as a
    // pointer (slot holds the malloc'd buffer); voperand emits the malloc
    // at scope entry and the parent TokenCpnd's cleanup emits the free.
    // dims[0] holds the element-count contribution from the FIRST dim
    // (always 1 for the runtime path; multiply by vla_size_expr at runtime).
    class TokenBase *vla_size_expr;
    // Parameter array bounds like `int a[n++]` decay to pointers but still
    // evaluate their runtime bound expressions on function entry.
    class TokenBase *param_vla_side_effect_expr;
    Variable() { type = &ddINT; data = NULL; aot_data_offset = (size_t)-1; aot_cstr_offset = (size_t)-1; flags = 0; count = 0; object_size_hint = -1; vla_size_expr = nullptr; param_vla_side_effect_expr = nullptr; }
    Variable(std::string n, DataDef &d, uint32_t c = 1, void *init=NULL, bool alloc=true);
   ~Variable();
    inline bool is_vla() const { return vla_size_expr != nullptr; }
    inline bool is_fixed_array() const { return (flags & vfFIXEDARRAY) != 0; }
    // A reference variable (`T& r`, `auto& x`, a `T&` parameter, a `for(T& v:c)`
    // loop var): its type is a DataDefREF. The single source of truth for
    // reference-ness — first-class refs Phase 2 retired the parallel vfREFERENCE
    // flag. The value auto-derefs on access; the slot holds the referent address.
    inline bool is_reference() const { return type && type->is_reference(); }
    inline bool has_aot_data() const { return aot_data_offset != (size_t)-1; }
    inline bool has_aot_cstr() const { return aot_cstr_offset != (size_t)-1; }
    inline size_t total_elements() const {
	size_t n = 1;
	for ( auto d : dims ) n *= d;
	return n;
    }
    inline void modified() { flags |= vfMODIFIED; DBG(std::cout << "Variable::modified(" << name << ')' << std::endl); }
    inline void makeconstant() { flags |= vfCONSTANT; }
    inline bool is_global()   const { if ( (flags & vfLOCAL) && !(flags &vfSTATIC) ) return false; return true; }
    inline bool is_constant() const { if ( (flags & vfCONSTANT) ) return true; return false; }
    bool set(int64_t c)
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   *((char *)data) = c;
	else if (type == &ddBOOL)   *((bool *)data) = c;
	// madc's `int` is 64-bit by design.  Writing only the low 4 bytes
	// via `(int *)` left the high half at zero (calloc'd 0), so e.g.
	// `enum { WEAR_NONE = -1 }; if (x == WEAR_NONE)` failed because
	// WEAR_NONE round-tripped as 0x00000000FFFFFFFF, not 0xFFFF..FFFF.
	else if (type == &ddINT)    *((int64_t *)data) = c;
	else if (type == &ddINT64)  *((int64_t *)data) = c;
	else if (type == &ddINT8)   *((int8_t *)data) = c;
	else if (type == &ddINT16)  *((int16_t *)data) = c;
	else if (type == &ddINT24)  *((int16_t *)data) = c;
	else if (type == &ddINT32)  *((int32_t *)data) = c;
	else if (type == &ddUINT8)  *((uint8_t *)data) = c;
	else if (type == &ddUINT16) *((uint16_t *)data) = c;
	else if (type == &ddUINT24) *((uint16_t *)data) = c;
	else if (type == &ddUINT32) *((uint32_t *)data) = c;
	else if (type == &ddUINT64) *((uint64_t *)data) = c;
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
	if (type == &ddINT)    return *((int64_t *)data) == c;
	if (type == &ddINT64)  return *((int64_t *)data) == c;
	if (type == &ddINT8)   return *((int8_t *)data) == c;
	if (type == &ddINT16)  return *((int16_t *)data) == c;
	if (type == &ddINT24)  return *((int16_t *)data) == c;
	if (type == &ddINT32)  return *((int32_t *)data) == c;
	if (type == &ddUINT8)  return *((uint8_t *)data) == static_cast<uint8_t>(c);
	if (type == &ddUINT16) return *((uint16_t *)data) == static_cast<uint16_t>(c);
	if (type == &ddUINT24) return *((uint16_t *)data) == static_cast<uint16_t>(c);
	if (type == &ddUINT32) return *((uint32_t *)data) == static_cast<uint32_t>(c);
	if (type == &ddUINT64) return *((uint64_t *)data) == static_cast<uint64_t>(c);
	if (type == &ddFLOAT)  return *((float *)data) == c;
	if (type == &ddDOUBLE) return *((double *)data) == c;
	return 0;
    }
    int cmp(std::string &s)
    {
	if (type == &ddCHARptr && data)
	{
	    const char *p = *(const char **)data;
	    return p ? std::strcmp(p, s.c_str()) : -1;
	}
	return 0;
    }
    bool dec()
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   --*((char *)data);
	else if (type == &ddINT)    --*((int64_t *)data);
	else if (type == &ddINT64)  --*((int64_t *)data);
	else if (type == &ddINT8)   --*((int8_t *)data);
	else if (type == &ddINT16)  --*((int16_t *)data);
	else if (type == &ddINT24)  --*((int16_t *)data);
	else if (type == &ddINT32)  --*((int32_t *)data);
	else if (type == &ddUINT8)  --*((uint8_t *)data);
	else if (type == &ddUINT16) --*((uint16_t *)data);
	else if (type == &ddUINT24) --*((uint16_t *)data);
	else if (type == &ddUINT32) --*((uint32_t *)data);
	else if (type == &ddUINT64) --*((uint64_t *)data);
	else if (type == &ddFLOAT)  --*((float *)data);
	else if (type == &ddDOUBLE) --*((double *)data);
	return true;
    }
    bool inc()
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   ++*((char *)data);
	else if (type == &ddINT)    ++*((int64_t *)data);
	else if (type == &ddINT64)  ++*((int64_t *)data);
	else if (type == &ddINT8)   ++*((int8_t *)data);
	else if (type == &ddINT16)  ++*((int16_t *)data);
	else if (type == &ddINT24)  ++*((int16_t *)data);
	else if (type == &ddINT32)  ++*((int32_t *)data);
	else if (type == &ddUINT8)  ++*((uint8_t *)data);
	else if (type == &ddUINT16) ++*((uint16_t *)data);
	else if (type == &ddUINT24) ++*((uint16_t *)data);
	else if (type == &ddUINT32) ++*((uint32_t *)data);
	else if (type == &ddUINT64) ++*((uint64_t *)data);
	else if (type == &ddFLOAT)  ++*((float *)data);
	else if (type == &ddDOUBLE) ++*((double *)data);
	return true;
    }
    template<typename T> T get()
    {
	if ( !data ) { return false; }
	/**/ if (type == &ddCHAR)   return *((char *)data);
	else if (type == &ddINT)    return *((int64_t *)data);
	else if (type == &ddINT64)  return *((int64_t *)data);
	else if (type == &ddINT8)   return *((int8_t *)data);
	else if (type == &ddINT16)  return *((int16_t *)data);
	else if (type == &ddINT24)  return *((int16_t *)data);
	else if (type == &ddINT32)  return *((int32_t *)data);
	else if (type == &ddUINT8)  return *((uint8_t *)data);
	else if (type == &ddUINT16) return *((uint16_t *)data);
	else if (type == &ddUINT24) return *((uint16_t *)data);
	else if (type == &ddUINT32) return *((uint32_t *)data);
	else if (type == &ddUINT64) return *((uint64_t *)data);
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
    virtual int64_t get() const { return var.get<int64_t>(); }
    virtual int val() const     { return var.get<int>(); }
    // Constant-fold path (TokenOperator::optimize → ioperate/foperate)
    // calls ival()/dval() on each leaf. Without these overrides
    // enum/const-var leaves report 0, so e.g. (TOPCOLOR-COLORBASE)
    // folds to 0 at runtime even though parse-time array sizing reads
    // them correctly via read_constant_integer.
    virtual int64_t ival() const override
        { return var.is_constant() ? var.get<int64_t>() : 0; }
    virtual double dval() const override
        { return var.is_constant() ? var.get<double>() : 0; }
    virtual bool is_constant() const { return var.is_constant(); }
    virtual bool is_real() const { return _datatype->is_real(); }
    virtual void set(int64_t c) { DBG(std::cout << "TokenVariable: set() calling var.set()" << std::endl); var.set(c); }
};

#endif // __TOKENDATA_H
