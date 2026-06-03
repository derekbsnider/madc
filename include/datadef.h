#ifndef __DATADEF_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Data definitions						//
//									//
//////////////////////////////////////////////////////////////////////////
#define __DATADEF_H 1

#include <cstdint>
#include <map>
#include <set>
#include <vector>

extern thread_local bool madc_verbose;
// JIT/codegen optimization level (0-3), set by the `-O<n>` CLI flag. Drives
// both MIR_gen_set_optimize_level and c2mir's compile optimize_level. Default 1.
extern thread_local int madc_opt_level;
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
	// std::string is no longer here — it is a generic class tagged dtRESERVED,
	// recognized by class identity (P2.14). dtISTREAM is now the first value
	// after dtRESERVED; the ptr/ref lists below shift in lockstep, preserving
	// the rtPtr(+10000)/rtRef(+20000) relationship.
	dtISTREAM = 256, dtOSTREAM, dtISSTREAM, dtOSSTREAM, dtSSTREAM,
	dtIFSTREAM, dtOFSTREAM, dtFSTREAM, dtTCPSTREAM,
	dtMUTEX, dtTHREAD, dtTHISTHREAD,
	dtARRAY,

	// rtPointer variants
	dtVOIDptr = 10000, dtBOOLptr, dtUINT8ptr, dtBYTEptr=dtUINT8ptr, dtINT8ptr, dtCHARptr = dtINT8ptr,
	dtUINT16ptr, dtINT16ptr, dtSHORTptr=dtINT16ptr, dtUINT24ptr, dtINT24ptr,
	dtUINT32ptr, dtINT32ptr, dtUINT64ptr, dtINT64ptr, dtINTptr=dtINT32ptr,
	dtFLOATptr, dtFLOAT32ptr=dtFLOATptr, dtDOUBLEptr, dtDOUBLE64ptr=dtDOUBLEptr,
	dtLDOUBLEptr, dtDOUBLE80ptr=dtLDOUBLEptr, dtRESERVEDptr = 10255,
	dtISTREAMptr = 10256, dtOSTREAMptr, dtISSTREAMptr, dtOSSTREAMptr, dtSSTREAMptr,
	dtIFSTREAMptr, dtOFSTREAMptr, dtFSTREAMptr, dtTCPSTREAMptr,
	dtMUTEXptr, dtTHREADptr, dtTHISTHREADptr,
	dtARRAYptr,

	// rtReference variants
	dtVOIDref = 20000, dtBOOLref, dtUINT8ref, dtBYTEref=dtUINT8ref, dtINT8ref, dtCHARref = dtINT8ref,
	dtUINT16ref, dtINT16ref, dtSHORTref=dtINT16ref, dtUINT24ref, dtINT24ref,
	dtUINT32ref, dtINT32ref, dtUINT64ref, dtINT64ref, dtINTref=dtINT32ref,
	dtFLOATref, dtFLOAT32ref=dtFLOATref, dtDOUBLEref, dtDOUBLE64ref=dtDOUBLEref,
	dtLDOUBLEref, dtDOUBLE80ref=dtLDOUBLEref, dtRESERVEDref = 20255,
	dtISTREAMref = 20256, dtOSTREAMref, dtISSTREAMref, dtOSSTREAMref, dtSSTREAMref,
	dtIFSTREAMref, dtOFSTREAMref, dtFSTREAMref, dtTCPSTREAMref,
	dtMUTEXref, dtTHREADref, dtTHISTHREADref,
	dtARRAYref,
};

// Variable flags
typedef enum : uint32_t { vfLOCAL	=    1, // local vs global
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
			  vfREFERENCE	=65536, // reference parameter (T&): auto-deref on access
			  vfCONSTDECL  =131072, // `const`-DECLARED var (vfCONSTANT for write
			                        // enforcement) whose value is NOT set() into
			                        // data — so it must NOT be read-fold-substituted
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
    // Canonical C++ type spelling for Itanium mangling, e.g.
    // "std::basic_ofstream<char,std::char_traits<char>>". Empty = use `name`
    // (or a builtin spelling). Set on a std:: template INSTANTIATION (see
    // instantiate_template_use) so a bodyless std:: method binds to the real
    // libstdc++ mangled symbol with NO hardcoded literal. (std::string default-
    // constructs empty, so the assignment-style ctors below need no change.)
    std::string	 canonical_cpp_spelling;
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
    // Class-identity marker for std::string. Recognizing std::string by a
    // dedicated raw type-code was the residual special-casing P2.14 removed — a
    // std::string is a real C++ class (like vector/map/set) and should be
    // recognized by its CLASS IDENTITY, not a reserved enum tag. The two
    // std::string DataDef classes (DataDefSTRING / DataDefSTRINGref) override
    // this to return true; everything else inherits false. The std::string tag is
    // now gone — DataDefSTRING carries the generic dtRESERVED class tag — so this
    // identity marker is the ONLY way string is recognized. Use the
    // is_std_string()/is_std_string_ref() free recognizers
    // (below) at call sites — they layer reftype() on top of this marker.
    virtual bool is_string_class() const { return false; }
    virtual bool is_string() const { return is_string_class(); }
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
};

// Member descriptor for struct/union/class layouts. Models a
// std::pair<name, type> (the .first/.second names are kept for
// backward compatibility) plus the source typedef alias used at the
// member's declaration site (empty for raw types), so the CIR layer
// can emit ID("alias") instead of the underlying type nodes.
struct memberpair_t {
    std::string first;          // member name
    DataDef *second;            // member type
    std::string typedef_name;   // source typedef alias, "" if raw type
    TokenBase *origin;          // member-name source token (CIR origin), NULL if none
    memberpair_t() : second(nullptr), origin(nullptr) {}
    memberpair_t(const std::string &n, DataDef *d) : first(n), second(d), origin(nullptr) {}
    memberpair_t(const std::string &n, DataDef *d, const std::string &td)
	: first(n), second(d), typedef_name(td), origin(nullptr) {}
};

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
    std::vector<std::vector<uint32_t>> member_dims;
    std::vector<TokenBase *> member_count_exprs;	// runtime-sized member count expr, or NULL
    std::vector<uint32_t> member_access;	// per-member access flags (0=public, vfPRIVATE, vfPROTECTED)
    std::vector<int> member_origin;	// per-member: base index it came from, or -1 = own (MI flatten)
    std::map<size_t,size_t> member_explicit_align; // member index -> __attribute__((aligned(N))); absent = natural
    TokenBase *runtime_size_expr;
    size_t pack;	// 0 = natural C ABI alignment, 1 = packed, N = max alignment N
    size_t max_align;	// largest member alignment (for finalizing struct size)
    size_t tag_explicit_align;	// __attribute__((aligned(N))) on the struct TAG (0 = none)
    bool union_layout;	// true: all members start at offset 0; size is max member size
    bool is_complete;	// true: a `{ ... }` body was parsed (even if empty) — distinguishes
			// `struct X {}` (complete, zero members) from `struct X;` (forward decl)
    bool has_anon_aggregate;	// true: addAnonymousAggregate() was used to flatten members
    bool is_anonymous = false;	// true: a tagless `struct {..}` / `union {..}` — `name` is a
				// UNIQUE synthetic tag (`__anon_N`) so distinct anonymous
				// aggregates don't collide in by-name dedup, while still
				// being a real, emittable, referenceable C tag.
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
	: DataDef(n, s, d), runtime_size_expr(NULL), pack(0), max_align(1), tag_explicit_align(0), union_layout(false),
	  is_complete(false), has_anon_aggregate(false),
	  reverse_scalar_storage(false), bitfield_active(false), bitfield_unit_offset(0),
	  bitfield_unit_size(0), bitfield_next_bit(0) {}
    DataDefSTRUCT(std::string n, std::vector<memberpair_t> m)
	: DataDef(n, 0, DataType::dtRESERVED), runtime_size_expr(NULL), pack(0), max_align(1), tag_explicit_align(0),
	  union_layout(false), is_complete(false), has_anon_aggregate(false),
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
	bool is_array_decl = false, const std::vector<uint32_t> *dims = NULL)
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
	    member_dims.push_back(dims ? *dims : std::vector<uint32_t>());
	    member_count_exprs.push_back(count_expr);
	    member_access.push_back(0);
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
	member_dims.push_back(dims ? *dims : std::vector<uint32_t>());
	member_count_exprs.push_back(count_expr);
	member_access.push_back(0);
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
		|| name == "long long"
		|| name == "int8_t"
		|| name == "int16_t"
		|| name == "int24_t"
		|| name == "int32_t"
		|| name == "int64_t";
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
	member_dims.push_back(std::vector<uint32_t>());
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
	has_anon_aggregate = true;
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
	    member_dims.push_back(i < agg.member_dims.size() ? agg.member_dims[i] : std::vector<uint32_t>());
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
    // Apply __attribute__((aligned(N))) to the most recently added member.
    // Updates the member's offset (re-aligns it to N) and the struct's
    // overall alignment requirement.
    void apply_member_alignment(size_t align)
    {
	if ( align == 0 || members.empty() ) return;
	if ( align > max_align ) max_align = align;
	// Record the requested per-member alignment so the CIR emitter can place
	// an _Alignas(N) on this member's spec (c2mir lays the field out; this
	// keeps madc's own offset/size table in agreement below).
	member_explicit_align[members.size() - 1] = align;
	if ( union_layout ) return; // unions don't have per-member offsets
	size_t idx = member_offsets.size() - 1;
	size_t old_ofs = member_offsets[idx];
	size_t new_ofs = align_up(old_ofs, align);
	if ( new_ofs != old_ofs )
	{
	    size_t delta = new_ofs - old_ofs;
	    member_offsets[idx] = new_ofs;
	    size += delta;
	}
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
    // Per-member access flag (0=public, vfPRIVATE, vfPROTECTED). Returns 0
    // (public) for an unknown member or a struct with no access info (a plain
    // C struct never sets member_access entries past the default 0).
    uint32_t m_access(const std::string &member) const
    {
	for ( size_t i = 0; i < members.size(); ++i )
	    if ( !member.compare(members[i].first) )
		return (i < member_access.size()) ? member_access[i] : 0;
	return 0;
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
    const std::vector<uint32_t> *m_dims(const std::string &member) const
    {
	for ( size_t i = 0; i < members.size(); ++i )
	    if ( !member.compare(members[i].first) )
		return (i < member_dims.size()) ? &member_dims[i] : NULL;
	return NULL;
    }
    bool has_runtime_size() const
    {
	for ( size_t i = 0; i < member_count_exprs.size(); ++i )
	    if ( member_count_exprs[i] != NULL )
		return true;
	return false;
    }
    // A C99 flexible array member (`T x[]`) or GNU zero-length array (`T x[0]`)
    // as the LAST struct member: an array declarator, not runtime-sized, whose
    // sole/leading dimension is 0. Such a member contributes no fixed size and
    // is sized by its initializer (file-scope object) — c2mir wants its array
    // declarator emitted with an UNSPECIFIED size (N_IGNORE), not a literal 0.
    bool m_is_flexible_array(const std::string &member) const
    {
	if ( members.empty() )
	    return false;
	for ( size_t i = 0; i < members.size(); ++i )
	    if ( !member.compare(members[i].first) )
	    {
		if ( i + 1 != members.size() )
		    return false; // only the trailing member can be flexible
		if ( i >= member_array_flags.size() || !member_array_flags[i] )
		    return false;
		if ( i < member_count_exprs.size() && member_count_exprs[i] )
		    return false; // runtime-sized, not flexible
		if ( i < member_dims.size() && member_dims[i].size() == 1
		    && member_dims[i][0] == 0 )
		    return true;
		return false;
	    }
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

class DataDefCLASS;
// A direct base of a class: the base type, its subobject offset within the
// derived object (Itanium-computed by compute_layout), whether it is a virtual
// base, its access (existing vf* flags), and whether it is the primary base
// (shares the most-derived vptr at offset 0).
struct BaseSpec {
    DataDefCLASS *base;
    size_t        offset;
    bool          is_virtual;
    uint32_t      access;
    bool          is_primary;
};

class DataDefCLASS: public DataDefSTRUCT
{
public:
    std::vector<Variable *> methods;
    std::vector<Variable *> staticconst;
    std::map<std::string, Variable *> method_map; // unmangled name -> method variable
    // Constructor overload set (each entry's FuncDef carries the param signature
    // and, for class-bound externals like std::string, an emit_symbol naming the
    // real ctor). A user class with one ClassName() ctor has a single entry; a
    // class with overloaded ctors (std::string: default / const char* / copy)
    // has several. Empty when the class has no recorded ctor.
    std::vector<Variable *> ctors;
    bool has_user_ctor;  // true if user defined ClassName() constructor
    bool has_user_dtor;  // true if user defined ~ClassName() destructor
    void *extern_ctor;   // C function pointer for extern class default constructor (NULL if none)
    void *extern_dtor;   // C function pointer for extern class destructor (NULL if none)
    void *_dtor_ptr;     // copy of extern_dtor; &_dtor_ptr is fn_indirect for cleanup stack
    DataDefCLASS *base_class; // single inheritance: parent class (NULL if none)
    // Multiple/virtual inheritance (Itanium layout). `bases` lists direct bases;
    // compute_layout() fills each BaseSpec.offset/is_primary, vbase_offset (each
    // shared virtual base -> its offset, appended once at the end),
    // secondary_vptr_owners (non-primary polymorphic direct bases), and nvsize
    // (size of the non-virtual portion, where virtual bases begin).
    std::vector<BaseSpec> bases;
    std::map<DataDefCLASS *, size_t> vbase_offset;
    std::vector<DataDefCLASS *> secondary_vptr_owners;
    size_t nvsize;
    size_t own_block_off; // offset where this class's own data members begin
    bool has_vptr_slot;   // class carries a vptr (virtual methods OR a virtual base); set by compute_layout
    bool is_polymorphic() const { return has_vtable; }
    void compute_layout(); // Itanium layout engine (defined in parser.cpp)
    void apply_member_layout(); // rewrite member_offsets from member_origin + computed layout
    // Collect all (transitive) virtual bases, deduped, in canonical order.
    void collect_vbases(std::vector<DataDefCLASS *> &out,
			std::set<DataDefCLASS *> &seen) const;
    // Virtual function table
    std::vector<std::string> vtable_slots; // method names in vtable slot order
    std::map<std::string, bool> virtual_methods;  // names of methods declared virtual
    void **vtable;         // runtime vtable (array of function pointers, filled at compile time)
    bool has_vtable;       // true if this class or any base has virtual methods
    int vtable_slot(const std::string &name) const {
	for ( size_t i = 0; i < vtable_slots.size(); ++i )
	    if ( vtable_slots[i] == name ) return (int)i;
	return -1;
    }
    bool is_virtual_method(const std::string &name) const {
	if ( virtual_methods.find(name) != virtual_methods.end() ) return true;
	if ( base_class ) return base_class->is_virtual_method(name);
	return false;
    }
    // Is this class the same as `target`, or derived (single-inheritance chain)
    // from it? Used for protected-member access and derived->base pointer
    // upcasts. Single inheritance only — the base subobject lives at offset 0.
    bool is_or_derives_from(const DataDefCLASS *target) const {
	for ( const DataDefCLASS *c = this; c; c = c->base_class )
	    if ( c == target ) return true;
	return false;
    }

    DataDefCLASS(std::string n, size_t s, DataType d)
	: DataDefSTRUCT(n, s, d), has_user_ctor(false), has_user_dtor(false),
	  extern_ctor(NULL), extern_dtor(NULL), _dtor_ptr(NULL),
	  base_class(NULL), nvsize(0), own_block_off(0), has_vptr_slot(false),
	  vtable(NULL), has_vtable(false) {}
    virtual BaseType basetype() const { return BaseType::btClass; }
    Variable *findMethod(std::string &s);
    void register_extern_ctor_dtor(void *ctor, void *dtor) {
	extern_ctor = ctor; extern_dtor = dtor; _dtor_ptr = dtor;
    }
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
// std::string is a GENERIC class (P2.14 complete): its DataDef carries the same
// generic class tag as vector/map/set and user classes — dtRESERVED for the
// value `string`, dtRESERVEDref for `string&` (so reftype()==rtReference still
// holds). There is NO dedicated std::string tag any more. std::string is recognized
// purely by CLASS IDENTITY via the is_string_class() virtual / the is_std_string*
// free recognizers, exactly like any other class. as_user_class excludes it (by
// identity) so it routes through as_object_class to its opaque-buffer + mangled
// libstdc++ ctor/dtor/method lowering rather than the plain-C-struct path.
class DataDefSTRING:    public DDClass { public: DataDefSTRING():  DDClass("string", sizeof(std::string), DataType::dtRESERVED) {} virtual bool is_string_class() const { return true; } };
class DataDefSTRINGref: public DDClass { public: DataDefSTRINGref(): DDClass("string&", sizeof(std::string &), DataType::dtRESERVEDref) {} virtual bool is_string_class() const { return true; } };
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
    // A pointer/ref to a std::string (`string*`, and the `string&` loop-var
    // model getPointerType(ddSTRING)+vfREFERENCE) carries the string identity
    // through its base_type. Mirrors the legacy is_string() which decoded the
    // std::string* rawtype to std::string. The value-vs-pointer distinction stays
    // via is_pointer() / the vfREFERENCE Variable flag, exactly as before P2.14.
    virtual bool is_string_class() const { return base_type && base_type->is_string_class(); }
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

class DataDefENUM : public DataDef
{
public:
    std::string enum_name;

    DataDefENUM(const std::string &name)
	: DataDef(name, sizeof(int), DataType::dtINT), enum_name(name) {}
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

// MadValue's own discriminator. The PHP/borrowed-language mixed-array union
// holds exactly these five alternatives; it formerly borrowed DataType enum
// values (dtVOID/dtINT64/dtDOUBLE/std::string/dtARRAY) as the tag, which is a
// different concept from DataDef type identity. A self-owned kind makes the
// union self-consistent and removes the last MadValue dependency on the
// std::string enum word (P2.14 chunk 2).
enum class MadValueKind : uint8_t { mvNONE, mvINT, mvDOUBLE, mvSTRING, mvARRAY };

struct MadValue
{
    MadValueKind kind;
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
    MadValue(const std::string &s) : kind(MadValueKind::mvSTRING)
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
    bool         is_string() const { return kind == MadValueKind::mvSTRING; }
    bool         is_int()    const { return kind == MadValueKind::mvINT; }
    bool         is_double() const { return kind == MadValueKind::mvDOUBLE; }
    bool         is_array()  const { return kind == MadValueKind::mvARRAY; }
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

inline MadValue::MadValue(const MadArray &a) : kind(MadValueKind::mvARRAY)
{
    ptr = new MadArray(a);
}

inline MadValue::MadValue() : kind(MadValueKind::mvNONE), ival(0) {}

inline MadValue::MadValue(int64_t v) : kind(MadValueKind::mvINT), ival(v) {}

inline MadValue::MadValue(double v) : kind(MadValueKind::mvDOUBLE), dval(v) {}

inline MadValue::MadValue(const MadValue &o) : kind(o.kind)
{
    if ( kind == MadValueKind::mvSTRING && o.ptr )
	ptr = new std::string(*(std::string *)o.ptr);
    else if ( kind == MadValueKind::mvARRAY && o.ptr )
	ptr = new MadArray(*(MadArray *)o.ptr);
    else
	ival = o.ival;
}

inline MadValue &MadValue::operator=(const MadValue &o)
{
    if ( this != &o )
    {
	if ( kind == MadValueKind::mvSTRING && ptr )
	    delete (std::string *)ptr;
	else if ( kind == MadValueKind::mvARRAY && ptr )
	    delete (MadArray *)ptr;
	kind = o.kind;
	if ( kind == MadValueKind::mvSTRING && o.ptr )
	    ptr = new std::string(*(std::string *)o.ptr);
	else if ( kind == MadValueKind::mvARRAY && o.ptr )
	    ptr = new MadArray(*(MadArray *)o.ptr);
	else
	    ival = o.ival;
    }
    return *this;
}

inline MadValue::~MadValue()
{
    if ( kind == MadValueKind::mvSTRING && ptr )
	delete (std::string *)ptr;
    else if ( kind == MadValueKind::mvARRAY && ptr )
	delete (MadArray *)ptr;
}

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

// ---- std::string class-identity recognizers (P2.14) -------------------------
// Recognize a std::string by CLASS IDENTITY (the is_string_class() virtual)
// rather than the raw std::string / std::string& type-code, so the recognition is
// principled and survives retiring those enum tags. These are the canonical
// replacements for the scattered `rawtype()==std::string` / `==std::string&`
// checks. A std::string DataDef may appear by value, by reference (T&), or by
// pointer (T*) — these helpers distinguish those forms via reftype().

// True for ANY std::string DataDef regardless of ref/pointer form (value,
// string&, string*). The broad "is this a string at all?" predicate.
static inline bool is_std_string(const DataDef *dd)
{
	return dd && dd->is_string_class();
}

// True only for a std::string REFERENCE (string& — DataDefSTRINGref, or a
// string DataDef whose reftype() is rtReference). Replaces the old
// `rawtype()==std::string&` checks. Note that the dedicated DataDefSTRINGref
// singleton answers rtReference via its tag today; after Phase 4 it carries
// the same identity marker, so this stays correct.
static inline bool is_std_string_ref(const DataDef *dd)
{
	return dd && dd->is_string_class()
	    && dd->reftype() == RefType::rtReference;
}

// True for a std::string VALUE object (not a reference, not a pointer) — the
// receiver that owns the operators/methods. Replaces `rawtype()==std::string`
// where the intent was specifically "a string value/object" (e.g. copy-ctor
// param matching, by-value receiver dispatch).
static inline bool is_std_string_value(const DataDef *dd)
{
	return dd && dd->is_string_class()
	    && dd->reftype() == RefType::rtValue;
}

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
    // True when the type was written with explicit pointer syntax — a value
    // `RET (*name)(params)` or a Form-2 typedef `typedef RET (*NAME)(params)`.
    // False for a Form-1 function typedef `typedef RET NAME(params)`, whose
    // alias names a bare function type (so `NAME f` declares a function and
    // `NAME *p` a pointer). Drives whether the CIR renderer parenthesizes the
    // typedef itself vs. defers the `*` to each use site.
    bool ptr_syntax;
    DataDefFPTR(FuncDef *fd) : DataDef("funcptr", 8, DataType::dtINT64), target(fd), ptr_syntax(true) {}
    virtual BaseType basetype() const { return BaseType::btFunct; }
    virtual bool is_function() const { return true; }
    virtual bool is_numeric()  const { return true; }
    virtual bool is_integer()  const { return true; }
};

#endif // __DATADEF_H
