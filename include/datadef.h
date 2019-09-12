#ifndef __DATADEF_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Data definitions						//
//									//
//////////////////////////////////////////////////////////////////////////
#define __DATADEF_H 1

enum class BaseType : uint8_t { btSimple, btStruct, btFunct, btClass     };
enum class RefType  : uint8_t { rtNone, rtValue, rtPointer, rtReference  };

enum class DataType : uint16_t {
	// Simple data types
	dtVOID, dtBOOL, dtUINT8, dtBYTE=dtUINT8,  dtINT8, dtCHAR = dtINT8,
	dtUINT16, dtINT16, dtSHORT=dtINT16, dtUINT24, dtINT24,
	dtUINT32, dtINT32, dtUINT64, dtINT64, dtINT=dtINT64,
	dtFLOAT, dtFLOAT32=dtFLOAT, dtDOUBLE, dtDOUBLE64=dtDOUBLE,
	dtLDOUBLE, dtDOUBLE80 = dtLDOUBLE, dtRESERVED = 255,
	// complex and valarray
	// some Standard C++ classes
	dtSTRING, dtISTREAM, dtOSTREAM, dtISSTREAM, dtOSSTREAM, dtSSTREAM,
	dtIFSTREAM, dtOFSTREAM, dtFSTREAM, dtTCPSTREAM, 
	dtMUTEX, dtTHREAD, dtTHISTHREAD,

	// rtPointer variants
	dtVOIDptr = 10000, dtBOOLptr, dtUINT8ptr, dtBYTEptr=dtUINT8ptr, dtINT8ptr, dtCHARptr = dtINT8ptr,
	dtUINT16ptr, dtINT16ptr, dtSHORTptr=dtINT16ptr, dtUINT24ptr, dtINT24ptr,
	dtUINT32ptr, dtINT32ptr, dtUINT64ptr, dtINT64ptr, dtINTptr=dtINT64ptr,
	dtFLOATptr, dtFLOAT32ptr=dtFLOATptr, dtDOUBLEptr, dtDOUBLE64ptr=dtDOUBLEptr,
	dtLDOUBLEptr, dtDOUBLE80ptr=dtLDOUBLEptr, dtRESERVEDptr = 10255,
	dtSTRINGptr, dtISTREAMptr, dtOSTREAMptr, dtISSTREAMptr, dtOSSTREAMptr, dtSSTREAMptr,
	dtIFSTREAMptr, dtOFSTREAMptr, dtFSTREAMptr, dtTCPSTREAMptr, 
	dtMUTEXptr, dtTHREADptr, dtTHISTHREADptr,

	// rtReference variants
	dtVOIDref = 20000, dtBOOLref, dtUINT8ref, dtBYTEref=dtUINT8ref, dtINT8ref, dtCHARref = dtINT8ref,
	dtUINT16ref, dtINT16ref, dtSHORTref=dtINT16ref, dtUINT24ref, dtINT24ref,
	dtUINT32ref, dtINT32ref, dtUINT64ref, dtINT64ref, dtINTref=dtINT64ref,
	dtFLOATref, dtFLOAT32ref=dtFLOATref, dtDOUBLEref, dtDOUBLE64ref=dtDOUBLEref,
	dtLDOUBLEref, dtDOUBLE80ref=dtLDOUBLEref, dtRESERVEDref = 20255,
	dtSTRINGref, dtISTREAMref, dtOSTREAMref, dtISSTREAMref, dtOSSTREAMref, dtSSTREAMref,
	dtIFSTREAMref, dtOFSTREAMref, dtFSTREAMref, dtTCPSTREAMref, 
	dtMUTEXref, dtTHREADref, dtTHISTHREADref,
};

// Variable flags
typedef enum : uint16_t { vfLOCAL	=    1, // local vs global
			  vfSTACK	=    2, // stack vs heap
			  vfSTATIC	=    4, // static variable
			  vfREGSET	=   64, // GpReg set
			  vfXREGSET	=  128, // extra reg is set (used for string.c_str)
			  vfALLOC	=  256, // data pointer was allocated by us
			  vfSTACKSET	=  512, // we reserved stack space
			  vfMODIFIED	= 1024, // variable was modified
			  vfCONSTANT	= 2048, // variable is a constant
			} varflag_t;

#define rtNone(x) 0
#define rtVal(x) (x)
#define rtPtr(x) (DataType)((uint32_t)x+10000)
#define rtRef(x) (DataType)((uint32_t)x+20000)
#define rtDePtr(x) (DataType)((uint32_t)x-10000)
#define rtDeRef(x) (DataType)((uint32_t)x-20000)

class DataDef
{
protected:
    uint32_t     _type;
public:
    std::string	 name;
    size_t	 size;
    DataDef() { size = 0; _type = 0; }
    DataDef(std::string n, size_t s, DataType d) { name = n; size = s; _type = (uint32_t)d; }
    virtual ~DataDef() {}
    virtual bool is_string() { if (rawtype() == DataType::dtSTRING) return true; return false; }
    virtual bool is_numeric()
    {
	if ( basetype() != BaseType::btSimple )
	    return false;
	if ( _type > 0 && _type < (uint16_t)DataType::dtRESERVED )
	    return true;
	return false;
    }
    virtual bool is_function()
    {
	if ( basetype() == BaseType::btFunct )
	    return true;
	return false;
    }
    virtual bool is_struct()
    {
	if ( basetype() == BaseType::btStruct )
	    return true;
	return false;
    }
    virtual bool is_object()
    {
	if ( basetype() == BaseType::btClass )
	    return true;
    	switch(rawtype())
    	{
	    case DataType::dtSTRING:
	    case DataType::dtOSTREAM:
	    case DataType::dtSSTREAM:
	    case DataType::dtOSSTREAM:
	    case DataType::dtFSTREAM:
	    case DataType::dtOFSTREAM:
	    case DataType::dtTCPSTREAM:
		return true;
	    default:
		return false;
    	}
	return false;
    }
    virtual bool has_ostream()
    {
    	switch(rawtype())
    	{
	    case DataType::dtOSTREAM:
	    case DataType::dtSSTREAM:
	    case DataType::dtOSSTREAM:
	    case DataType::dtFSTREAM:
	    case DataType::dtOFSTREAM:
	    case DataType::dtTCPSTREAM:
		return true;
	    default:
		return false;
    	}
	return false;
    }
    virtual DataType type() const { return (DataType)_type; }
    virtual BaseType basetype() const { return BaseType::btSimple; }
    static inline DataType rawtype(DataType dt)
    {
	if ((uint32_t)dt >= 20000) { return (DataType)((uint32_t)dt-20000); }
	if ((uint32_t)dt >= 10000) { return (DataType)((uint32_t)dt-10000); }
	return dt;
    }
    virtual DataType rawtype()  const 
    {
	if (_type >= 20000) { return (DataType)(_type-20000); }
	if (_type >= 10000) { return (DataType)(_type-10000); }
	return (DataType)_type;
    }
    virtual RefType reftype()  const
    {
	if (_type >= 20000) { return RefType::rtReference; }
	if (_type >= 10000) { return RefType::rtPointer;   }
	return RefType::rtValue;
    }
    virtual void setRef(RefType rt)
    {
	/**/ if (_type >= 20000) { if (rt==RefType::rtReference) return; _type -= 20000; }
	else if (_type >= 10000) { if (rt==RefType::rtPointer)   return; _type -= 10000; }
	if ( rt == RefType::rtReference ) { _type += 20000; return; }
	if ( rt == RefType::rtPointer )   { _type += 10000; return; }
    }
};

// base type tokens
class TokenBaseType: public TokenIdent
{
public:
    DataDef &definition;
    TokenBaseType(const char *k, DataDef &d) : TokenIdent(k), definition(d) {}
    virtual TokenType type() const { return TokenType::ttBaseType; }
    virtual TokenBase *clone() { return new TokenBaseType(str.c_str(), definition); }
};


class Variable; // forward dec

class DataDefSTRUCT: public DataDef
{
public:
    DataDefSTRUCT(std::string n) : DataDef(n, 0, DataType::dtRESERVED) {}
    virtual BaseType basetype() const { return BaseType::btStruct; }
    std::vector<Variable *> members;
};

class DataDefCLASS: public DataDef
{
public:
    DataDefCLASS(std::string n, size_t s, DataType d) : DataDef(n, s, d) {}
    DataDefCLASS(std::string n, DataType d, std::vector<DataDef> m) : DataDef(n, 0, d)
    {
	std::vector<DataDef>::iterator dvi;
	for ( dvi = m.begin(); dvi != m.end(); ++dvi )
	    addMember(*dvi);
    }
    virtual ~DataDefCLASS()
    {
	std::vector<DataDef *>::iterator dvi;
	for ( dvi = members.begin(); dvi != members.end(); ++dvi )
	    delete *dvi;
    }
    virtual BaseType basetype() const { return BaseType::btClass; }
    std::vector<DataDef *> members;
    std::vector<Variable *> methods;
    std::vector<Variable *> staticconst;
    void addMember(DataDef &dd) { addMember(dd.name, dd.size, dd.type()); }
    void addMember(std::string n, size_t s, DataType d)
    {
	DataDef *dd = new DataDef(n, s, d);
	size += s;
	members.push_back(dd);
    }
};

typedef DataDefCLASS DDClass;

// data definitions of default base types
class DataDefVOID:      public DataDef { public: DataDefVOID() :   DataDef("void", 0,     DataType::dtVOID) {} };
class DataDefVOIDref:   public DataDef { public: DataDefVOIDref(): DataDef("void&", 0,    DataType::dtVOIDref) {} };
class DataDefBOOL:      public DataDef { public: DataDefBOOL() :   DataDef("bool", 1,     DataType::dtBOOL) {} };
class DataDefCHAR:      public DataDef { public: DataDefCHAR() :   DataDef("char", 1,     DataType::dtCHAR) {} };
class DataDefINT:       public DataDef { public: DataDefINT()  :   DataDef("int",  8,     DataType::dtINT) {} };
class DataDefINT8:      public DataDef { public: DataDefINT8() :   DataDef("int8_t", 1,   DataType::dtINT8) {} };
class DataDefINT16:     public DataDef { public: DataDefINT16():   DataDef("int16_t", 2,  DataType::dtINT16) {} };
class DataDefINT24:     public DataDef { public: DataDefINT24():   DataDef("int24_t", 3,  DataType::dtINT24) {} };
class DataDefINT32:     public DataDef { public: DataDefINT32():   DataDef("int32_t", 4,  DataType::dtINT32) {} };
class DataDefINT64:     public DataDef { public: DataDefINT64():   DataDef("int64_t", 8,  DataType::dtINT64) {} };
class DataDefUINT8:     public DataDef { public: DataDefUINT8() :  DataDef("uint8_t", 1,  DataType::dtUINT8) {} };
class DataDefUINT16:    public DataDef { public: DataDefUINT16():  DataDef("uint16_t", 2, DataType::dtUINT16) {} };
class DataDefUINT24:    public DataDef { public: DataDefUINT24():  DataDef("uint24_t", 3, DataType::dtUINT24) {} };
class DataDefUINT32:    public DataDef { public: DataDefUINT32():  DataDef("uint32_t", 4, DataType::dtUINT32) {} };
class DataDefUINT64:    public DataDef { public: DataDefUINT64():  DataDef("uint64_t", 8, DataType::dtUINT64) {} };
class DataDefFLOAT:     public DataDef { public: DataDefFLOAT() :  DataDef("float", 4,    DataType::dtFLOAT) {} };
class DataDefDOUBLE:    public DataDef { public: DataDefDOUBLE():  DataDef("double", 8,   DataType::dtDOUBLE) {} };
class DataDefSTRING:    public DDClass { public: DataDefSTRING():  DDClass("string", sizeof(std::string), DataType::dtSTRING) {} };
class DataDefSTRINGref: public DDClass { public: DataDefSTRINGref(): DDClass("string&", sizeof(std::string &), DataType::dtSTRINGref) {} };
class DataDefOSTREAM:   public DDClass { public: DataDefOSTREAM(): DDClass("ostream", sizeof(std::ostream), DataType::dtOSTREAM) {} };
class DataDefSSTREAM:   public DDClass { public: DataDefSSTREAM(): DDClass("stringstream", sizeof(std::stringstream), DataType::dtSSTREAM) {} };
class DataDefLPSTR:     public DataDef { public: DataDefLPSTR():   DataDef("LPSTR", sizeof(char *), rtPtr(DataType::dtCHAR)) {} };

#if 0
class DataDefTEST:      public DDClass { public: DataDefTEST():
	DDClass("testclass", DataType::dtRESERVED,
	{
		{"name", sizeof(std::string), DataType::dtSTRING},
		{"age",  sizeof(uint8_t), DataType::dtUINT8},
		{"sex",  sizeof(uint8_t), DataType::dtUINT8}
	}) {}
};
#endif

extern DataDefVOID ddVOID;
extern DataDefVOIDref ddVOIDref;
extern DataDefBOOL ddBOOL;
extern DataDefCHAR ddCHAR;
extern DataDefINT ddINT;
extern DataDefINT8 ddINT8;
extern DataDefINT16 ddINT16;
extern DataDefINT24 ddINT24;
extern DataDefINT32 ddINT32;
extern DataDefINT64 ddINT64;
extern DataDefUINT8 ddUINT8;
extern DataDefUINT16 ddUINT16;
extern DataDefUINT24 ddUINT24;
extern DataDefUINT32 ddUINT32;
extern DataDefUINT64 ddUINT64;
extern DataDefFLOAT ddFLOAT;
extern DataDefDOUBLE ddDOUBLE;
extern DataDefSTRING ddSTRING;
extern DataDefSTRINGref ddSTRINGref;
extern DataDefLPSTR ddLPSTR;
extern DataDefOSTREAM ddOSTREAM;
extern DataDefSSTREAM ddSSTREAM;

// token definitions of default base types
class TokenVOID:      public TokenBaseType { public: TokenVOID() :  TokenBaseType("void", ddVOID) {} };
class TokenBOOL:      public TokenBaseType { public: TokenBOOL() :  TokenBaseType("bool", ddBOOL) {} };
class TokenCHAR:      public TokenBaseType { public: TokenCHAR() :  TokenBaseType("char", ddCHAR) {} };
class TokenINT:       public TokenBaseType { public: TokenINT()  :  TokenBaseType("int", ddINT) {} };
class TokenINT8:      public TokenBaseType { public: TokenINT8() :  TokenBaseType("int8_t", ddINT8) {} };
class TokenINT16:     public TokenBaseType { public: TokenINT16():  TokenBaseType("int16_t", ddINT16) {} };
class TokenINT24:     public TokenBaseType { public: TokenINT24():  TokenBaseType("int24_t", ddINT24) {} };
class TokenINT32:     public TokenBaseType { public: TokenINT32():  TokenBaseType("int32_t", ddINT32) {} };
class TokenINT64:     public TokenBaseType { public: TokenINT64():  TokenBaseType("int64_t", ddINT64) {} };
class TokenUINT8:     public TokenBaseType { public: TokenUINT8() : TokenBaseType("uint8_t", ddUINT8) {} };
class TokenUINT16:    public TokenBaseType { public: TokenUINT16(): TokenBaseType("uint16_t", ddUINT16) {} };
class TokenUINT24:    public TokenBaseType { public: TokenUINT24(): TokenBaseType("uint24_t", ddUINT24) {} };
class TokenUINT32:    public TokenBaseType { public: TokenUINT32(): TokenBaseType("uint32_t", ddUINT32) {} };
class TokenUINT64:    public TokenBaseType { public: TokenUINT64(): TokenBaseType("uint64_t", ddUINT64) {} };
class TokenFLOAT:     public TokenBaseType { public: TokenFLOAT() : TokenBaseType("float", ddFLOAT) {} };
class TokenDOUBLE:    public TokenBaseType { public: TokenDOUBLE(): TokenBaseType("double", ddDOUBLE) {} };
class TokenSTRING:    public TokenBaseType { public: TokenSTRING(): TokenBaseType("string", ddSTRING) {} };
class TokenOSTREAM:   public TokenBaseType { public: TokenOSTREAM(): TokenBaseType("ostream", ddOSTREAM) {} };
class TokenSSTREAM:   public TokenBaseType { public: TokenSSTREAM(): TokenBaseType("stringstream", ddSSTREAM) {} };
class TokenLPSTR:     public TokenBaseType { public: TokenLPSTR():  TokenBaseType("LPSTR", ddLPSTR) {} };

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
    TokenVar(Variable &v) : TokenBase(), var(v) {}
    virtual TokenType type() const { return TokenType::ttVariable; }
    virtual int get() const { return var.get<int>(); }
    virtual int val() const { return var.get<int>(); }
    virtual void set(int c) { DBG(std::cout << "TokenVariable: set() calling var.set()" << std::endl); var.set(c); }
    virtual asmjit::x86::Gp &getreg(Program &);
    virtual void putreg(Program &);
    virtual asmjit::x86::Gp &compile(Program &, asmjit::x86::Gp *ret=NULL);
};

#endif // __DATADEF_H
