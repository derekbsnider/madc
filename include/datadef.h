#ifndef __DATADEF_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Data definitions						//
//									//
//////////////////////////////////////////////////////////////////////////
#define __DATADEF_H 1

extern thread_local bool madc_verbose;
class TokenBase;

enum class BaseType : uint8_t { btSimple, btStruct, btFunct, btClass     };
enum class RefType  : uint8_t { rtNone, rtValue, rtPointer, rtReference  };

enum class DataType : uint16_t {
	// Simple data types
	dtVOID, dtBOOL, dtUINT8, dtBYTE=dtUINT8,  dtINT8, dtCHAR = dtINT8,
	dtUINT16, dtINT16, dtSHORT=dtINT16, dtUINT24, dtINT24,
	dtUINT32, dtINT32, dtUINT64, dtINT64, dtINT=dtINT32,
	dtFLOAT=12, dtFLOAT32=dtFLOAT, dtDOUBLE, dtDOUBLE64=dtDOUBLE,
	dtLDOUBLE, dtDOUBLE80 = dtLDOUBLE, dtSIMD, dtRESERVED = 255,
	// complex and valarray
	// some Standard C++ classes
	dtSTRING, dtISTREAM, dtOSTREAM, dtISSTREAM, dtOSSTREAM, dtSSTREAM,
	dtIFSTREAM, dtOFSTREAM, dtFSTREAM, dtTCPSTREAM, 
	dtMUTEX, dtTHREAD, dtTHISTHREAD,
	dtARRAY,
	dtVECTOR, dtMAP, dtSET, dtLIST,

	// rtPointer variants
	dtVOIDptr = 10000, dtBOOLptr, dtUINT8ptr, dtBYTEptr=dtUINT8ptr, dtINT8ptr, dtCHARptr = dtINT8ptr,
	dtUINT16ptr, dtINT16ptr, dtSHORTptr=dtINT16ptr, dtUINT24ptr, dtINT24ptr,
	dtUINT32ptr, dtINT32ptr, dtUINT64ptr, dtINT64ptr, dtINTptr=dtINT32ptr,
	dtFLOATptr, dtFLOAT32ptr=dtFLOATptr, dtDOUBLEptr, dtDOUBLE64ptr=dtDOUBLEptr,
	dtLDOUBLEptr, dtDOUBLE80ptr=dtLDOUBLEptr, dtRESERVEDptr = 10255,
	dtSTRINGptr, dtISTREAMptr, dtOSTREAMptr, dtISSTREAMptr, dtOSSTREAMptr, dtSSTREAMptr,
	dtIFSTREAMptr, dtOFSTREAMptr, dtFSTREAMptr, dtTCPSTREAMptr, 
	dtMUTEXptr, dtTHREADptr, dtTHISTHREADptr,
	dtARRAYptr,
	dtVECTORptr, dtMAPptr, dtSETptr, dtLISTptr,

	// rtReference variants
	dtVOIDref = 20000, dtBOOLref, dtUINT8ref, dtBYTEref=dtUINT8ref, dtINT8ref, dtCHARref = dtINT8ref,
	dtUINT16ref, dtINT16ref, dtSHORTref=dtINT16ref, dtUINT24ref, dtINT24ref,
	dtUINT32ref, dtINT32ref, dtUINT64ref, dtINT64ref, dtINTref=dtINT32ref,
	dtFLOATref, dtFLOAT32ref=dtFLOATref, dtDOUBLEref, dtDOUBLE64ref=dtDOUBLEref,
	dtLDOUBLEref, dtDOUBLE80ref=dtLDOUBLEref, dtRESERVEDref = 20255,
	dtSTRINGref, dtISTREAMref, dtOSTREAMref, dtISSTREAMref, dtOSSTREAMref, dtSSTREAMref,
	dtIFSTREAMref, dtOFSTREAMref, dtFSTREAMref, dtTCPSTREAMref, 
	dtMUTEXref, dtTHREADref, dtTHISTHREADref,
	dtARRAYref,
	dtVECTORref, dtMAPref, dtSETref, dtLISTref,
};

// Variable flags
typedef enum : uint16_t { vfLOCAL	=    1, // local vs global
			  vfSTACK	=    2, // stack vs heap
			  vfSTATIC	=    4, // static variable
			  vfPARAM	=    8, // parameter variable
			  vfREGISTER	=   16, // register-only: never written to memory
			  vfFIXEDARRAY	=   32, // C fixed-size array (var.dims non-empty)
			  vfREGSET	=   64, // GpReg set
			  vfXREGSET	=  128, // extra reg is set (used for string.c_str)
			  vfALLOC	=  256, // data pointer was allocated by us
			  vfSTACKSET	=  512, // we reserved stack space
			  vfMODIFIED	= 1024, // variable was modified
			  vfCONSTANT	= 2048, // variable is a constant
			  vfPRIVATE	= 4096, // variable is a private class member
			  vfPROTECTED	= 8192, // variable is a protected class member
			  vfADDRTAKEN	=16384, // variable needs stable stack storage for &
			  vfEXTERN	=32768, // extern declaration placeholder
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
    virtual bool is_compatible(DataDef &d)
    {
	if ( &d == this
	||   rawtype() == d.rawtype()
	||  (is_numeric() && d.is_numeric()) )
	    return true;

	return false;
    }
    virtual bool is_string() const { if (rawtype() == DataType::dtSTRING) return true; return false; }
    virtual bool is_complex() const { return false; }
    virtual bool is_numeric() const
    {
	if ( basetype() != BaseType::btSimple )
	    return false;
	if ( _type > 0 && _type < (uint16_t)DataType::dtRESERVED )
	    return true;
	return false;
    }
    virtual bool is_integer() const
    {
	if ( basetype() != BaseType::btSimple )
	    return false;
	if ( _type > 0 && _type < (uint16_t)DataType::dtFLOAT )
	    return true;
	return false;
    }
    virtual bool is_real() const
    {
	if ( basetype() != BaseType::btSimple )
	    return false;
	if ( _type >= (uint16_t)DataType::dtFLOAT && _type < (uint16_t)DataType::dtRESERVED )
	    return true;
	return false;
    }
    virtual bool is_simd() const { return false; }
    virtual bool is_unsigned() const
    {
    	switch(rawtype())
    	{
	    case DataType::dtUINT8:
	    case DataType::dtUINT16:
	    case DataType::dtUINT24:
	    case DataType::dtUINT32:
	    case DataType::dtUINT64:
		return true;
	    default:
		return false;
    	}
	return false;
    }
    virtual bool is_function() const
    {
	if ( basetype() == BaseType::btFunct )
	    return true;
	return false;
    }
    virtual bool is_pointer() const
    {
	return _type >= 10000 && _type < 20000;
    }
    virtual bool is_struct() const
    {
	if ( basetype() == BaseType::btStruct )
	    return true;
	return false;
    }
    virtual bool is_object() const
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
	    case DataType::dtIFSTREAM:
	    case DataType::dtISTREAM:
	    case DataType::dtTCPSTREAM:
	    case DataType::dtARRAY:
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
    virtual bool has_istream()
    {
    	switch(rawtype())
    	{
	    case DataType::dtISTREAM:
	    case DataType::dtSSTREAM:
	    case DataType::dtISSTREAM:
	    case DataType::dtFSTREAM:
	    case DataType::dtIFSTREAM:
		return true;
	    default:
		return false;
    	}
	return false;
    }
    virtual size_t alignment() const
    {
	if ( basetype() == BaseType::btStruct )
	    return size;
	if ( size == 0 )
	    return 1;
	return size > 8 ? 8 : size;
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
    // get a new register for the datatype
    virtual asmjit::Operand newreg(asmjit::x86::Compiler &cc, const char *n=NULL)
    {
	// IMPORTANT: asmjit's newGpq/newXmm/etc. are variadic printf-style —
	// they interpret the first const char* as a format string. Names that
	// contain '%' (e.g. our __literal__... string-literal variable names
	// with embedded %ld / %s) must be passed with an explicit "%s" fmt
	// to avoid garbage deref on the unmatched format spec.
	switch((DataType)_type)
	{
	// Sub-32-bit types use 64-bit registers with sign/zero-extension
	// on load because 8-bit and 16-bit x86 ops do NOT clear upper bits.
	case DataType::dtCHAR:
	case DataType::dtBOOL:
	case DataType::dtINT16:
	case DataType::dtINT24:
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT24:
	case DataType::dtINT64:
	case DataType::dtUINT64:  return n ? cc.newGpq("%s", n) : cc.newGpq();
	// 32-bit types use 32-bit registers. On x86-64 all 32-bit ops
	// automatically zero-extend to 64 bits, so wrapping at 2^32 is
	// free and upper bits are always clean.
	case DataType::dtINT32:
	case DataType::dtUINT32:  return n ? cc.newGpd("%s", n) : cc.newGpd();
	case DataType::dtFLOAT:   return n ? cc.newXmm("%s", n) : cc.newXmm();
	case DataType::dtDOUBLE:  return n ? cc.newXmm("%s", n) : cc.newXmm();
	case DataType::dtLDOUBLE: return n ? cc.newXmm("%s", n) : cc.newXmm();
	case DataType::dtSIMD:    return n ? cc.newXmm("%s", n) : cc.newXmm();
	default:		  return n ? cc.newIntPtr("%s", n) : cc.newIntPtr();
	}
    }
    // move a register value into memory location pointed to by a pointer
    // mov([mem], reg)
    virtual void movrval2mptr(asmjit::x86::Compiler &cc, void *ptr, asmjit::x86::Gp reg)
    {
	switch((DataType)_type)
	{
	case DataType::dtCHAR:    cc.mov(asmjit::x86::byte_ptr((uintptr_t)ptr),  reg); break;
	case DataType::dtBOOL:    cc.mov(asmjit::x86::byte_ptr((uintptr_t)ptr),  reg); break;
	case DataType::dtINT64:   cc.mov(asmjit::x86::qword_ptr((uintptr_t)ptr), reg); break;
	case DataType::dtINT16:   cc.mov(asmjit::x86::word_ptr((uintptr_t)ptr),  reg); break;
	case DataType::dtINT24:   cc.mov(asmjit::x86::word_ptr((uintptr_t)ptr),  reg); break;
	case DataType::dtINT32:   cc.mov(asmjit::x86::dword_ptr((uintptr_t)ptr), reg); break;
	case DataType::dtUINT8:   cc.mov(asmjit::x86::byte_ptr((uintptr_t)ptr),  reg); break;
	case DataType::dtUINT16:  cc.mov(asmjit::x86::word_ptr((uintptr_t)ptr),  reg); break;
	case DataType::dtUINT24:  cc.mov(asmjit::x86::word_ptr((uintptr_t)ptr),  reg); break;
	case DataType::dtUINT32:  cc.mov(asmjit::x86::dword_ptr((uintptr_t)ptr), reg); break;
	case DataType::dtUINT64:  cc.mov(asmjit::x86::qword_ptr((uintptr_t)ptr), reg); break;
	case DataType::dtFLOAT:   cc.mov(asmjit::x86::qword_ptr((uintptr_t)ptr), reg); break;
	case DataType::dtDOUBLE:  cc.mov(asmjit::x86::qword_ptr((uintptr_t)ptr), reg); break;
	case DataType::dtLDOUBLE: cc.mov(asmjit::x86::qword_ptr((uintptr_t)ptr), reg); break;
	default: DBG(std::cerr << "DataDef::putreg() unsupported numeric type " << _type << std::endl); break;
	}
    }
    // move a register value into a memory location pointed to by a register
    // mov([reg1], reg2)
    virtual void movrval2rptr(asmjit::x86::Compiler &cc, asmjit::x86::Gp ptr, asmjit::x86::Gp reg)
    {
	switch((DataType)_type)
	{
	case DataType::dtCHAR:    cc.mov(asmjit::x86::byte_ptr(ptr),  reg); break;
	case DataType::dtBOOL:    cc.mov(asmjit::x86::byte_ptr(ptr),  reg); break;
	case DataType::dtINT64:   cc.mov(asmjit::x86::qword_ptr(ptr), reg); break;
	case DataType::dtINT16:   cc.mov(asmjit::x86::word_ptr(ptr),  reg); break;
	case DataType::dtINT24:   cc.mov(asmjit::x86::word_ptr(ptr),  reg); break;
	case DataType::dtINT32:   cc.mov(asmjit::x86::dword_ptr(ptr), reg); break;
	case DataType::dtUINT8:   cc.mov(asmjit::x86::byte_ptr(ptr),  reg); break;
	case DataType::dtUINT16:  cc.mov(asmjit::x86::word_ptr(ptr),  reg); break;
	case DataType::dtUINT24:  cc.mov(asmjit::x86::word_ptr(ptr),  reg); break;
	case DataType::dtUINT32:  cc.mov(asmjit::x86::dword_ptr(ptr), reg); break;
	case DataType::dtUINT64:  cc.mov(asmjit::x86::qword_ptr(ptr), reg); break;
	default:		  cc.mov(asmjit::x86::ptr(ptr), reg);       break;
	// DBG(std::cerr << "DataDef::movrval2rptr() unsupported numeric type " << _type << std::endl); break;
	}
    }
    // move a numeric value into a memory location pointed to by a register
    // mov([reg1], int)
    virtual void movint2rptr(asmjit::x86::Compiler &cc, asmjit::x86::Gp ptr, int rval)
    {
	switch((DataType)_type)
	{
	case DataType::dtCHAR:    cc.mov(asmjit::x86::byte_ptr(ptr),  rval); break;
	case DataType::dtBOOL:    cc.mov(asmjit::x86::byte_ptr(ptr),  rval); break;
	case DataType::dtINT64:   cc.mov(asmjit::x86::qword_ptr(ptr), rval); break;
	case DataType::dtINT16:   cc.mov(asmjit::x86::word_ptr(ptr),  rval); break;
	case DataType::dtINT24:   cc.mov(asmjit::x86::word_ptr(ptr),  rval); break;
	case DataType::dtINT32:   cc.mov(asmjit::x86::dword_ptr(ptr), rval); break;
	case DataType::dtUINT8:   cc.mov(asmjit::x86::byte_ptr(ptr),  rval); break;
	case DataType::dtUINT16:  cc.mov(asmjit::x86::word_ptr(ptr),  rval); break;
	case DataType::dtUINT24:  cc.mov(asmjit::x86::word_ptr(ptr),  rval); break;
	case DataType::dtUINT32:  cc.mov(asmjit::x86::dword_ptr(ptr), rval); break;
	case DataType::dtUINT64:  cc.mov(asmjit::x86::qword_ptr(ptr), rval); break;
	default:		  cc.mov(asmjit::x86::ptr(ptr), rval);       break;
	// DBG(std::cerr << "DataDef::movint2rptr() unsupported numeric type " << _type << std::endl); break;
	}
    }
    // move memory pointed by a pointer into a Gp register
    // mov(reg, [mem])
    virtual void movmptr2rval(asmjit::x86::Compiler &cc, asmjit::x86::Gp &reg, void *ptr)
    {
	// Sub-64-bit loads from absolute addresses need a two-step approach:
	// load the address into a temp, then movsx/movzx from [temp].
	// Direct absolute-address movsx can't encode 64-bit displacements,
	// and the moffs mov only writes the sub-register (leaving upper
	// bits stale).
	using namespace asmjit;
	using namespace asmjit::x86;
	bool need_extend = false;
	switch((DataType)_type)
	{
	case DataType::dtCHAR:
	case DataType::dtBOOL:
	case DataType::dtINT16:
	case DataType::dtINT24:
	case DataType::dtINT32:
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT24:
	case DataType::dtUINT32:
	    need_extend = true;
	    break;
	default:
	    break;
	}
	if ( need_extend )
	{
	    Gp tmp = cc.newIntPtr("mptr_base");
	    cc.mov(tmp, imm((uintptr_t)ptr));
	    switch((DataType)_type)
	    {
	    case DataType::dtCHAR:    cc.movsx(reg, byte_ptr(tmp));   break;
	    case DataType::dtBOOL:    cc.movzx(reg, byte_ptr(tmp));   break;
	    case DataType::dtINT16:
	    case DataType::dtINT24:   cc.movsx(reg, word_ptr(tmp));   break;
	    case DataType::dtINT32:   cc.movsxd(reg, dword_ptr(tmp)); break;
	    case DataType::dtUINT8:   cc.movzx(reg, byte_ptr(tmp));   break;
	    case DataType::dtUINT16:
	    case DataType::dtUINT24:  cc.movzx(reg, word_ptr(tmp));   break;
	    case DataType::dtUINT32:  cc.mov(reg.r32(), dword_ptr(tmp)); break;
	    default: break;
	    }
	}
	else
	{
	    switch((DataType)_type)
	    {
	    case DataType::dtINT64:   cc.mov(reg, qword_ptr((uintptr_t)ptr)); break;
	    case DataType::dtUINT64:  cc.mov(reg, qword_ptr((uintptr_t)ptr)); break;
	    case DataType::dtFLOAT:   cc.mov(reg, dword_ptr((uintptr_t)ptr)); break;
	    case DataType::dtDOUBLE:  cc.mov(reg, qword_ptr((uintptr_t)ptr)); break;
	    case DataType::dtLDOUBLE: cc.mov(reg, tword_ptr((uintptr_t)ptr)); break;
	    default:		      cc.mov(reg, imm(ptr));		      break;
	    }
	}
    }
    // move memory pointed by a pointer into an Xmm register
    // mov(reg, [mem])
    //
    // Heap pointers from calloc usually live above the 32-bit signed
    // displacement range, so emitting `movss/movsd xmm, [abs64]`
    // directly trips asmjit's reloc-out-of-range error at finalize.
    // Spill the address into a Gp first and use register-base
    // addressing — the encoding for `movss/movsd xmm, [reg]` exists
    // for any 64-bit address. Float (4) needs movss; double (8) and
    // smaller-int loads happen via movsd which reads only the low
    // bytes of the addressed slot.
    virtual void movmptr2xval(asmjit::x86::Compiler &cc, asmjit::x86::Xmm &reg, void *ptr)
    {
	asmjit::x86::Gp p = cc.newIntPtr("_x_addr");
	cc.mov(p, asmjit::imm(ptr));
	switch((DataType)_type)
	{
	case DataType::dtCHAR:    cc.movsd(reg, asmjit::x86::byte_ptr(p));  break;
	case DataType::dtBOOL:    cc.movsd(reg, asmjit::x86::byte_ptr(p));  break;
	case DataType::dtINT64:   cc.movsd(reg, asmjit::x86::qword_ptr(p)); break;
	case DataType::dtINT16:   cc.movsd(reg, asmjit::x86::word_ptr(p));  break;
	case DataType::dtINT24:   cc.movsd(reg, asmjit::x86::word_ptr(p));  break;
	case DataType::dtINT32:   cc.movsd(reg, asmjit::x86::dword_ptr(p)); break;
	case DataType::dtUINT8:   cc.movsd(reg, asmjit::x86::byte_ptr(p));  break;
	case DataType::dtUINT16:  cc.movsd(reg, asmjit::x86::word_ptr(p));  break;
	case DataType::dtUINT24:  cc.movsd(reg, asmjit::x86::word_ptr(p));  break;
	case DataType::dtUINT32:  cc.movsd(reg, asmjit::x86::dword_ptr(p)); break;
	case DataType::dtUINT64:  cc.movsd(reg, asmjit::x86::qword_ptr(p)); break;
	case DataType::dtFLOAT:   cc.movss(reg, asmjit::x86::dword_ptr(p)); break;
	case DataType::dtDOUBLE:  cc.movsd(reg, asmjit::x86::qword_ptr(p)); break;
	case DataType::dtLDOUBLE: cc.movsd(reg, asmjit::x86::tword_ptr(p)); break;
	default:		  cc.movq(reg, asmjit::x86::qword_ptr(p));   break;
	} // switch
    }
    // move an Xmm register's value into the memory pointed to by a
    // pointer. Counterpart of movmptr2xval; needed by TokenCpnd::putreg
    // for write-back of double/float global variables. Same Gp-base
    // trick to dodge the 32-bit-displacement reloc limit.
    virtual void movxval2mptr(asmjit::x86::Compiler &cc, void *ptr, asmjit::x86::Xmm reg)
    {
	asmjit::x86::Gp p = cc.newIntPtr("_x_addr");
	cc.mov(p, asmjit::imm(ptr));
	switch((DataType)_type)
	{
	case DataType::dtFLOAT:   cc.movss(asmjit::x86::dword_ptr(p), reg); break;
	case DataType::dtDOUBLE:  cc.movsd(asmjit::x86::qword_ptr(p), reg); break;
	case DataType::dtLDOUBLE: cc.movsd(asmjit::x86::tword_ptr(p), reg); break;
	default:		  cc.movsd(asmjit::x86::qword_ptr(p), reg); break;
	}
    }
    // move memory pointed to by a register into a register
    virtual void movrptr2rval(asmjit::x86::Compiler &cc, asmjit::x86::Gp &reg, asmjit::x86::Gp &ptr)
    {
	switch((DataType)_type)
	{
	case DataType::dtCHAR:    cc.movsx(reg, asmjit::x86::byte_ptr(ptr));  break;
	case DataType::dtBOOL:    cc.movzx(reg, asmjit::x86::byte_ptr(ptr));  break;
	case DataType::dtINT64:   cc.mov(reg, asmjit::x86::qword_ptr(ptr));   break;
	case DataType::dtINT16:   cc.movsx(reg, asmjit::x86::word_ptr(ptr));  break;
	case DataType::dtINT24:   cc.movsx(reg, asmjit::x86::word_ptr(ptr));  break;
	case DataType::dtINT32:   cc.movsxd(reg, asmjit::x86::dword_ptr(ptr));break;
	case DataType::dtUINT8:   cc.movzx(reg, asmjit::x86::byte_ptr(ptr));  break;
	case DataType::dtUINT16:  cc.movzx(reg, asmjit::x86::word_ptr(ptr));  break;
	case DataType::dtUINT24:  cc.movzx(reg, asmjit::x86::word_ptr(ptr));  break;
	case DataType::dtUINT32:  cc.mov(reg.r32(), asmjit::x86::dword_ptr(ptr)); break;
	case DataType::dtUINT64:  cc.mov(reg, asmjit::x86::qword_ptr(ptr));   break;
	default:		  cc.mov(reg, asmjit::x86::ptr(ptr));          break;
	} // switch
    }
    // move memory pointed to by a register and an offset, into a register
    virtual void movrptr2rval(asmjit::x86::Compiler &cc, asmjit::x86::Gp &reg, asmjit::x86::Gp &ptr, size_t ofs)
    {
	switch((DataType)_type)
	{
	case DataType::dtCHAR:    cc.movsx(reg, asmjit::x86::byte_ptr(ptr, ofs));  break;
	case DataType::dtBOOL:    cc.movzx(reg, asmjit::x86::byte_ptr(ptr, ofs));  break;
	case DataType::dtINT64:   cc.mov(reg, asmjit::x86::qword_ptr(ptr, ofs));   break;
	case DataType::dtINT16:   cc.movsx(reg, asmjit::x86::word_ptr(ptr, ofs));  break;
	case DataType::dtINT24:   cc.movsx(reg, asmjit::x86::word_ptr(ptr, ofs));  break;
	case DataType::dtINT32:   cc.movsxd(reg, asmjit::x86::dword_ptr(ptr, ofs));break;
	case DataType::dtUINT8:   cc.movzx(reg, asmjit::x86::byte_ptr(ptr, ofs));  break;
	case DataType::dtUINT16:  cc.movzx(reg, asmjit::x86::word_ptr(ptr, ofs));  break;
	case DataType::dtUINT24:  cc.movzx(reg, asmjit::x86::word_ptr(ptr, ofs));  break;
	case DataType::dtUINT32:  cc.mov(reg.r32(), asmjit::x86::dword_ptr(ptr, ofs)); break;
	case DataType::dtUINT64:  cc.mov(reg, asmjit::x86::qword_ptr(ptr, ofs));   break;
	default:		  cc.mov(reg, asmjit::x86::ptr(ptr, ofs));          break;
	} // switch
    }
};

typedef std::pair<std::string, DataDef *> memberpair_t;

class Variable; // forward dec

class DataDefSTRUCT: public DataDef
{
public:
    struct BitFieldInfo
    {
	bool is_bitfield;
	size_t storage_offset;
	size_t storage_size;
	size_t bit_offset;
	size_t bit_width;
	bool is_unsigned;
	bool reverse_storage;
	BitFieldInfo()
	    : is_bitfield(false), storage_offset(0), storage_size(0),
	      bit_offset(0), bit_width(0), is_unsigned(false),
	      reverse_storage(false) {}
    };

    std::vector<memberpair_t> members;
    std::vector<size_t> member_counts;	// per-member count for fixed arrays (1 for scalars)
    std::vector<bool> member_array_flags;	// true when declarator used [] even if count folded to 1
    std::vector<size_t> member_offsets;	// per-member byte offset in the finalized layout
    std::vector<BitFieldInfo> member_bitfields;
    std::vector<TokenBase *> member_count_exprs;	// runtime-sized member count expr, or NULL
    TokenBase *runtime_size_expr;
    size_t pack;	// 0 = natural C ABI alignment, 1 = packed, N = max alignment N
    size_t max_align;	// largest member alignment (for finalizing struct size)
    bool union_layout;	// true: all members start at offset 0; size is max member size
    bool reverse_scalar_storage;
    bool bitfield_active;
    size_t bitfield_unit_offset;
    size_t bitfield_unit_size;
    size_t bitfield_next_bit;

    static size_t align_up(size_t v, size_t a) { return a ? ((v + a - 1) & ~(a - 1)) : v; }

    // compute alignment for a field: natural alignment capped by pack setting
    size_t field_align(const DataDef &dd) const
    {
	size_t natural = dd.alignment();
	if ( natural == 0 ) natural = 1;
	if ( pack == 0 ) return natural;              // C ABI default
	return pack < natural ? pack : natural;       // #pragma pack(N) caps alignment
    }

//    DataDefSTRUCT(std::string n) : DataDef(n, 0, DataType::dtRESERVED) {}
    DataDefSTRUCT(std::string n, size_t s, DataType d=DataType::dtRESERVED)
	: DataDef(n, s, d), runtime_size_expr(NULL), pack(0), max_align(1), union_layout(false),
	  reverse_scalar_storage(false), bitfield_active(false), bitfield_unit_offset(0),
	  bitfield_unit_size(0), bitfield_next_bit(0) {}
    DataDefSTRUCT(std::string n, std::vector<memberpair_t> m)
	: DataDef(n, 0, DataType::dtRESERVED), runtime_size_expr(NULL), pack(0), max_align(1),
	  union_layout(false),
	  reverse_scalar_storage(false), bitfield_active(false), bitfield_unit_offset(0),
	  bitfield_unit_size(0), bitfield_next_bit(0)
    {
	DBG(std::cout << "DataDefSTRUCT(" << n << ") constructor" << std::endl);
	std::vector<memberpair_t>::iterator dvpi;
	for ( dvpi = m.begin(); dvpi != m.end(); ++dvpi )
	    addMember(dvpi->first, *dvpi->second, 1);
	finalize();
	DBG(std::cout << "DataDefSTRUCT(" << n << ") members.size() " << members.size() << std::endl);
    }
    virtual ~DataDefSTRUCT()
    {
    }
    virtual BaseType basetype() const { return BaseType::btStruct; }
    virtual size_t alignment() const { return max_align ? max_align : 1; }
    void addMember(memberpair_t p) { addMember(p.first, *p.second, 1); }
    void setReverseScalarStorage(bool reverse)
    {
	reverse_scalar_storage = reverse;
	size_t unit_offset = (size_t)-1;
	size_t unit_size = 0;
	size_t unit_next_bit = 0;
	for ( size_t i = 0; i < member_bitfields.size(); ++i )
	{
	    BitFieldInfo &info = member_bitfields[i];
	    if ( !info.is_bitfield )
		continue;
	    if ( info.storage_offset != unit_offset || info.storage_size != unit_size )
	    {
		unit_offset = info.storage_offset;
		unit_size = info.storage_size;
		unit_next_bit = 0;
	    }
	    size_t storage_bits = info.storage_size * 8;
	    info.bit_offset = reverse_scalar_storage
		? (storage_bits - unit_next_bit - info.bit_width)
		: unit_next_bit;
	    info.reverse_storage = reverse_scalar_storage;
	    unit_next_bit += info.bit_width;
	}
    }
    void endBitFieldRun()
    {
	bitfield_active = false;
	bitfield_unit_offset = 0;
	bitfield_unit_size = 0;
	bitfield_next_bit = 0;
    }
    void addMember(std::string n, DataDef &dd, size_t cnt, TokenBase *count_expr = NULL,
	bool is_array_decl = false)
    {
	DBG(std::cout << "DataDefSTRUCT::addMember(" << n << ") at offset " << size << std::endl);
	endBitFieldRun();
	size_t fa = field_align(dd);
	if ( union_layout )
	{
	    if ( fa > max_align ) max_align = fa;
	    members.emplace_back(n, &dd);
	    member_counts.push_back(cnt);
	    member_array_flags.push_back(is_array_decl || count_expr != NULL);
	    member_offsets.push_back(0);
	    member_bitfields.push_back(BitFieldInfo());
	    member_count_exprs.push_back(count_expr);
	    size_t member_size = count_expr ? 0 : (dd.size * cnt);
	    if ( member_size > size ) size = member_size;
	    return;
	}
	size = align_up(size, fa);	// pad to field's alignment
	if ( fa > max_align ) max_align = fa;
	members.emplace_back(n, &dd);
	member_counts.push_back(cnt);
	member_array_flags.push_back(is_array_decl || count_expr != NULL);
	member_offsets.push_back(size);
	member_bitfields.push_back(BitFieldInfo());
	member_count_exprs.push_back(count_expr);
	if ( !count_expr )
	    size += dd.size * cnt;
    }
    size_t bitfield_storage_size(const DataDef &dd) const
    {
	size_t storage_size = dd.size ? dd.size : 4;
	return storage_size > 8 ? 8 : storage_size;
    }
    BitFieldInfo allocateBitField(DataDef &dd, size_t width)
    {
	auto is_builtin_signed_integer_name = [](const std::string &name) -> bool {
	    return name == "char"
		|| name == "short"
		|| name == "int"
		|| name == "long"
		|| name == "long long";
	};
	size_t storage_size = bitfield_storage_size(dd);
	size_t storage_bits = storage_size * 8;
	if ( !bitfield_active
	  || bitfield_unit_size != storage_size
	  || bitfield_next_bit + width > storage_bits )
	{
	    size_t fa = field_align(dd);
	    size = align_up(size, fa);
	    if ( fa > max_align ) max_align = fa;
	    bitfield_active = true;
	    bitfield_unit_offset = size;
	    bitfield_unit_size = storage_size;
	    bitfield_next_bit = 0;
	    size += storage_size;
	}

	BitFieldInfo info;
	info.is_bitfield = true;
	info.storage_offset = bitfield_unit_offset;
	info.storage_size = storage_size;
	info.bit_offset = reverse_scalar_storage
	    ? (storage_bits - bitfield_next_bit - width)
	    : bitfield_next_bit;
	info.bit_width = width;
	bool alias_like_int =
	    dd.rawtype() == DataType::dtINT32
	    && !is_builtin_signed_integer_name(dd.name);
	info.is_unsigned = dd.is_unsigned()
	    || dd.rawtype() == DataType::dtBOOL
	    || alias_like_int;
	info.reverse_storage = reverse_scalar_storage;
	bitfield_next_bit += width;
	if ( bitfield_next_bit >= storage_bits )
	    endBitFieldRun();
	return info;
    }
    void addBitField(std::string n, DataDef &dd, size_t width)
    {
	BitFieldInfo info = allocateBitField(dd, width);
	members.emplace_back(n, &dd);
	member_counts.push_back(1);
	member_array_flags.push_back(false);
	member_offsets.push_back(info.storage_offset);
	member_bitfields.push_back(info);
	member_count_exprs.push_back(NULL);
    }
    void addUnnamedBitField(DataDef &dd, size_t width)
    {
	if ( width == 0 )
	{
	    endBitFieldRun();
	    size_t fa = field_align(dd);
	    size = align_up(size, fa);
	    if ( fa > max_align ) max_align = fa;
	    return;
	}
	(void)allocateBitField(dd, width);
    }
    void addAnonymousAggregate(const DataDefSTRUCT &agg)
    {
	endBitFieldRun();
	size_t fa = field_align(agg);
	size_t base_offset = union_layout ? 0 : align_up(size, fa);
	if ( fa > max_align ) max_align = fa;
	for ( size_t i = 0; i < agg.members.size(); ++i )
	{
	    members.push_back(agg.members[i]);
	    member_counts.push_back(i < agg.member_counts.size() ? agg.member_counts[i] : 1);
	    member_array_flags.push_back(i < agg.member_array_flags.size() ? agg.member_array_flags[i] : false);
	    size_t child_offset = i < agg.member_offsets.size() ? agg.member_offsets[i] : 0;
	    member_offsets.push_back(base_offset + child_offset);
	    BitFieldInfo info = i < agg.member_bitfields.size() ? agg.member_bitfields[i] : BitFieldInfo();
	    if ( info.is_bitfield )
		info.storage_offset += base_offset;
	    member_bitfields.push_back(info);
	    member_count_exprs.push_back(i < agg.member_count_exprs.size() ? agg.member_count_exprs[i] : NULL);
	}
	size_t end = base_offset + agg.size;
	if ( union_layout )
	{
	    if ( end > size ) size = end;
	}
	else
	    size = end;
    }
    // round struct size up to its overall alignment (for arrays of structs)
    void finalize()
    {
	endBitFieldRun();
	size = align_up(size, max_align);
    }
    ssize_t m_offset(std::string &member)
    {
	DBG(std::cout << "DataDefSTRUCT::offset(" << member << ')' << std::endl);
	for ( size_t i = 0; i < members.size(); ++i )
	{
	    size_t ofs = (i < member_offsets.size()) ? member_offsets[i] : 0;
	    DBG(std::cout << "DataDefSTRUCT::offset(" << member << ") looking at " << members[i].first << " ofs=" << ofs << std::endl);
	    if ( !member.compare(members[i].first) )
		return (ssize_t)ofs;
	}
	return -1;
    }
    // Per-member fixed-array count (1 for scalar / non-array members).
    size_t m_count(std::string &member)
    {
	for ( size_t i = 0; i < members.size(); ++i )
	    if ( !member.compare(members[i].first) )
		return (i < member_counts.size()) ? member_counts[i] : 1;
	return 1;
    }
    TokenBase *m_count_expr(const std::string &member) const
    {
	for ( size_t i = 0; i < members.size(); ++i )
	    if ( !member.compare(members[i].first) )
		return (i < member_count_exprs.size()) ? member_count_exprs[i] : NULL;
	return NULL;
    }
    bool m_is_array_decl(const std::string &member) const
    {
	for ( size_t i = 0; i < members.size(); ++i )
	    if ( !member.compare(members[i].first) )
		return (i < member_array_flags.size()) ? member_array_flags[i] : false;
	return false;
    }
    bool has_runtime_size() const
    {
	for ( size_t i = 0; i < member_count_exprs.size(); ++i )
	    if ( member_count_exprs[i] != NULL )
		return true;
	return false;
    }
    DataDef *m_type(std::string &member)
    {
	std::vector<memberpair_t>::iterator dvpi;
	DBG(std::cout << "DataDefSTRUCT::type(" << member << ')' << std::endl);
	for ( dvpi = members.begin(); dvpi != members.end(); ++dvpi )
	{
	    DBG(std::cout << "DataDefSTRUCT::type(" << member << ") looking at " << dvpi->first << std::endl);
	    if ( !member.compare(dvpi->first) )
	    {
		DBG(std::cout << "DataDefSTRUCT::type() returning value " << (uint64_t)dvpi->second << std::endl);
		return dvpi->second;;
	    }
	}
	DBG(std::cout << "DataDefSTRUCT::type() returning NULL" << std::endl);
	return NULL;
    }
    const BitFieldInfo *m_bitfield(const std::string &member) const
    {
	for ( size_t i = 0; i < members.size(); ++i )
	    if ( !member.compare(members[i].first) )
		return (i < member_bitfields.size() && member_bitfields[i].is_bitfield)
		    ? &member_bitfields[i] : NULL;
	return NULL;
    }
    bool m_is_bitfield(const std::string &member) const
    {
	return m_bitfield(member) != NULL;
    }
};

class DataDefCLASS: public DataDefSTRUCT
{
public:
    std::vector<Variable *> methods;
    std::vector<Variable *> staticconst;
    std::map<std::string, Variable *> method_map; // unmangled name -> method variable

    DataDefCLASS(std::string n, size_t s, DataType d) : DataDefSTRUCT(n, s, d) {}
    virtual BaseType basetype() const { return BaseType::btClass; }
    Variable *findMethod(std::string &s);
};

typedef DataDefCLASS DDClass;

// data definitions of default base types
class DataDefVOID:      public DataDef { public: DataDefVOID() :   DataDef("void", 0,     DataType::dtVOID) {} };
class DataDefVOIDref:   public DataDef { public: DataDefVOIDref(): DataDef("void&", 0,    DataType::dtVOIDref) {} };
class DataDefBOOL:      public DataDef { public: DataDefBOOL() :   DataDef("bool", 1,     DataType::dtBOOL) {} };
class DataDefCHAR:      public DataDef { public: DataDefCHAR() :   DataDef("char", 1,     DataType::dtCHAR) {} };
class DataDefINT:       public DataDef { public: DataDefINT()  :   DataDef("int",  4,     DataType::dtINT) {} };
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
class DataDefISTREAM:   public DDClass { public: DataDefISTREAM(): DDClass("istream", sizeof(std::istream), DataType::dtISTREAM) {} };
class DataDefOSTREAM:   public DDClass { public: DataDefOSTREAM(): DDClass("ostream", sizeof(std::ostream), DataType::dtOSTREAM) {} };
class DataDefSSTREAM:   public DDClass { public: DataDefSSTREAM(): DDClass("stringstream", sizeof(std::stringstream), DataType::dtSSTREAM) {} };
class DataDefIFSTREAM:  public DDClass { public: DataDefIFSTREAM():DDClass("ifstream", sizeof(std::ifstream), DataType::dtIFSTREAM) {} };
class DataDefOFSTREAM:  public DDClass { public: DataDefOFSTREAM():DDClass("ofstream", sizeof(std::ofstream), DataType::dtOFSTREAM) {} };
class DataDefFSTREAM:   public DDClass { public: DataDefFSTREAM(): DDClass("fstream", sizeof(std::fstream), DataType::dtFSTREAM) {} };
class DataDefLPSTR:     public DataDef { public: DataDefLPSTR():   DataDef("LPSTR", sizeof(char *), rtPtr(DataType::dtCHAR)) {} };

// generic pointer-to-type — tracks what the pointer points to
// pointers are 64-bit integers at the ABI level (stored in Gp registers)
class DataDefPTR : public DataDef
{
public:
    DataDef *base_type;  // what this pointer points to
    DataDefPTR(DataDef &base)
	: DataDef(base.name + "*", 8, rtPtr(base.type())), base_type(&base) {}
    virtual bool is_pointer() const { return true; }
    virtual bool is_numeric() const { return true; }
    virtual bool is_integer() const { return true; }
    // Pointers are 8 bytes regardless of what they point to. Without these
    // overrides the base-class switches fall into the "unsupported numeric
    // type" default for rtPtr() values (>= 10000), silently dropping the
    // store — so `global_ptr = x;` becomes a no-op.
    virtual void movrval2mptr(asmjit::x86::Compiler &cc, void *ptr, asmjit::x86::Gp reg)
    { cc.mov(asmjit::x86::qword_ptr((uintptr_t)ptr), reg); }
    virtual void movrval2rptr(asmjit::x86::Compiler &cc, asmjit::x86::Gp ptr, asmjit::x86::Gp reg)
    { cc.mov(asmjit::x86::qword_ptr(ptr), reg); }
    virtual void movint2rptr(asmjit::x86::Compiler &cc, asmjit::x86::Gp ptr, int rval)
    { cc.mov(asmjit::x86::qword_ptr(ptr), rval); }
    virtual void movmptr2rval(asmjit::x86::Compiler &cc, asmjit::x86::Gp &reg, void *ptr)
    { cc.mov(reg, asmjit::x86::qword_ptr((uintptr_t)ptr)); }
};

class DataDefCArray : public DataDef
{
public:
    DataDef *element_type;
    size_t count;
    TokenBase *count_expr;

    DataDefCArray(DataDef &elem, const std::string &alias_name,
		  size_t cnt, TokenBase *expr = NULL)
	: DataDef(alias_name, expr ? 0 : (elem.size * cnt), DataType::dtRESERVED),
	  element_type(&elem), count(cnt), count_expr(expr) {}

    virtual size_t alignment() const
    {
	return element_type ? element_type->alignment() : DataDef::alignment();
    }

    bool has_runtime_size() const { return count_expr != NULL; }
};

class DataDefCOMPLEX : public DataDefSTRUCT
{
public:
    DataDef *element_type;

    DataDefCOMPLEX(DataDef &elem)
	: DataDefSTRUCT(elem.name + " _Complex", 0), element_type(&elem)
    {
	addMember("__real", elem, 1);
	addMember("__imag", elem, 1);
    }

    virtual bool is_complex() const { return true; }

    virtual bool is_compatible(DataDef &d)
    {
	if ( &d == this )
	    return true;
	DataDefCOMPLEX *other = dynamic_cast<DataDefCOMPLEX *>(&d);
	return other && other->element_type == element_type;
    }

    size_t component_offset(bool imag_part) const
    {
	std::string member = imag_part ? "__imag" : "__real";
	return const_cast<DataDefCOMPLEX *>(this)->m_offset(member);
    }
};

class MadArray;

// ---- MadValue: tagged union for PHP-style mixed-type arrays ----

struct MadValue
{
    DataType type;
    union {
	int64_t     ival;
	double      dval;
	void       *ptr;    // std::string*, MadArray*, etc.
    };

    MadValue();
    MadValue(int64_t v);
    MadValue(double v);
    MadValue(const MadArray &a);

    // string constructor — copies the string
    MadValue(const std::string &s) : type(DataType::dtSTRING)
    {
	ptr = new std::string(s);
    }

    // copy constructor — deep copy strings
    MadValue(const MadValue &o);

    // assignment — deep copy strings
    MadValue &operator=(const MadValue &o);

    ~MadValue();

    // accessors
    int64_t      as_int()    const { return ival; }
    double       as_double() const { return dval; }
    std::string &as_string() const { return *(std::string *)ptr; }
    MadArray    &as_array()  const { return *(MadArray *)ptr; }
    bool         is_string() const { return type == DataType::dtSTRING; }
    bool         is_int()    const { return type == DataType::dtINT64; }
    bool         is_double() const { return type == DataType::dtDOUBLE; }
    bool         is_array()  const { return type == DataType::dtARRAY; }
};

// PHP-style array: ordered, mixed-type, supports both integer and string keys
class MadArray
{
public:
    std::vector<MadValue> data;                      // indexed storage
    std::vector<std::pair<std::string, MadValue>> assoc; // associative storage

    size_t count() const { return data.size() + assoc.size(); }

    // integer-indexed access
    void push(const MadValue &v) { data.push_back(v); }
    MadValue &at(size_t i) { return data.at(i); }
    const MadValue &at(size_t i) const { return data.at(i); }

    // associative access
    MadValue &get(const std::string &key)
    {
	for ( auto &p : assoc )
	    if ( p.first == key ) return p.second;
	assoc.emplace_back(key, MadValue());
	return assoc.back().second;
    }

    void set(const std::string &key, const MadValue &v)
    {
	for ( auto &p : assoc )
	    if ( p.first == key ) { p.second = v; return; }
	assoc.emplace_back(key, v);
    }

    MadValue pop()
    {
	if ( data.empty() ) return MadValue();
	MadValue v = data.back();
	data.pop_back();
	return v;
    }
};

class DataDefARRAY:    public DDClass { public: DataDefARRAY():   DDClass("array", sizeof(MadArray), DataType::dtARRAY) {} };

inline MadValue::MadValue(const MadArray &a) : type(DataType::dtARRAY)
{
    ptr = new MadArray(a);
}

inline MadValue::MadValue() : type(DataType::dtVOID), ival(0) {}

inline MadValue::MadValue(int64_t v) : type(DataType::dtINT64), ival(v) {}

inline MadValue::MadValue(double v) : type(DataType::dtDOUBLE), dval(v) {}

inline MadValue::MadValue(const MadValue &o) : type(o.type)
{
    if ( type == DataType::dtSTRING && o.ptr )
	ptr = new std::string(*(std::string *)o.ptr);
    else if ( type == DataType::dtARRAY && o.ptr )
	ptr = new MadArray(*(MadArray *)o.ptr);
    else
	ival = o.ival;
}

inline MadValue &MadValue::operator=(const MadValue &o)
{
    if ( this != &o )
    {
	if ( type == DataType::dtSTRING && ptr )
	    delete (std::string *)ptr;
	else if ( type == DataType::dtARRAY && ptr )
	    delete (MadArray *)ptr;
	type = o.type;
	if ( type == DataType::dtSTRING && o.ptr )
	    ptr = new std::string(*(std::string *)o.ptr);
	else if ( type == DataType::dtARRAY && o.ptr )
	    ptr = new MadArray(*(MadArray *)o.ptr);
	else
	    ival = o.ival;
    }
    return *this;
}

inline MadValue::~MadValue()
{
    if ( type == DataType::dtSTRING && ptr )
	delete (std::string *)ptr;
    else if ( type == DataType::dtARRAY && ptr )
	delete (MadArray *)ptr;
}

// typed STL containers — parameterized types created lazily during parsing
class DataDefVECTOR: public DDClass
{
public:
    DataDef *element_type;
    DataDefVECTOR(DataDef *elem, const std::string &name, size_t sz)
	: DDClass(name, sz, DataType::dtVECTOR), element_type(elem) {}
};

class DataDefSIMD: public DataDef
{
public:
    DataDef *element_type;
    size_t vector_bytes;
    size_t lane_count;
    DataDefSIMD(DataDef *elem, const std::string &name, size_t bytes)
	: DataDef(name, bytes, DataType::dtSIMD), element_type(elem),
	  vector_bytes(bytes), lane_count((elem && elem->size) ? (bytes / elem->size) : 0) {}
    virtual bool is_numeric() const { return true; }
    virtual bool is_integer() const { return element_type && element_type->is_integer(); }
    virtual bool is_real() const { return element_type && element_type->is_real(); }
    virtual bool is_simd() const { return true; }
    virtual size_t alignment() const
    {
	if ( size >= 16 ) return 16;
	if ( size >= 8 ) return 8;
	return size ? size : 1;
    }
};

class DataDefMAP: public DDClass
{
public:
    DataDef *key_type;
    DataDef *val_type;
    DataDefMAP(DataDef *k, DataDef *v, const std::string &name, size_t sz)
	: DDClass(name, sz, DataType::dtMAP), key_type(k), val_type(v) {}
};

class DataDefSET: public DDClass
{
public:
    DataDef *element_type;
    DataDefSET(DataDef *elem, const std::string &name, size_t sz)
	: DDClass(name, sz, DataType::dtSET), element_type(elem) {}
};

class DataDefLIST: public DDClass
{
public:
    DataDef *element_type;
    DataDefLIST(DataDef *elem, const std::string &name, size_t sz)
	: DDClass(name, sz, DataType::dtLIST), element_type(elem) {}
};

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
extern DataDefPTR ddVOIDptr, ddCHARptr, ddINTptr, ddINT32ptr;
extern DataDefISTREAM ddISTREAM;
extern DataDefOSTREAM ddOSTREAM;
extern DataDefSSTREAM ddSSTREAM;
extern DataDefIFSTREAM ddIFSTREAM;
extern DataDefOFSTREAM ddOFSTREAM;
extern DataDefFSTREAM ddFSTREAM;
extern DataDefARRAY ddARRAY;

#if 1
class DataDefTEST:      public DataDefSTRUCT { public: DataDefTEST():
	DataDefSTRUCT("teststruct",
	{
		{"name", &ddSTRING},
		{"id",   &ddINT},
		{"age",  &ddUINT8},
		{"sex",  &ddUINT8}
	}) {}
};

extern DataDefTEST ddTESTSTRUCT;
#endif

// auto type placeholder
class DataDefAUTO: public DataDef
{
public:
    DataDefAUTO() : DataDef("auto", 0, DataType::dtVOID) {}
};
extern DataDefAUTO ddAUTO;

// function pointer type — wraps a FuncDef to carry the target signature
class FuncDef;  // forward declaration (defined in madc.h)
class DataDefFPTR: public DataDef
{
public:
    FuncDef *target;
    DataDefFPTR(FuncDef *fd) : DataDef("funcptr", 8, DataType::dtINT64), target(fd) {}
    virtual BaseType basetype() const { return BaseType::btFunct; }
    virtual bool is_function() const { return true; }
    virtual bool is_numeric()  const { return true; }
    virtual bool is_integer()  const { return true; }
};

#endif // __DATADEF_H
