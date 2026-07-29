#ifndef __DATADEF_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Data definitions						//
//									//
//////////////////////////////////////////////////////////////////////////
#define __DATADEF_H 1

#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <vector>

// madc::value — THE one tagged value type (libmadc embedding API and the
// script-side `array` / `madc::array` builtin alike). Depends only on the
// standard library; no include cycle.
#include "libmadc/value.h"

// Canonical typeid segments + ABI-pinned primitive slots (pure C header).
#include "madc_typeid.h"

extern thread_local bool madc_verbose;
#ifndef DBG
#define DBG(x) do { if (madc_verbose) { x; } } while (0)
#endif
// JIT/codegen optimization level (0-3), set by the `-O<n>` CLI flag. Drives
// both MIR_gen_set_optimize_level and c2mir's compile optimize_level. Default 1.
extern thread_local int madc_opt_level;
// `-g`: source-level debug info for the JIT lane. Stamps c2mir source
// locations, forces debuggable codegen (O0, no inlining, spill-all), and
// registers a GDB-JIT debug object after link so gdb can break/step/inspect
// JIT'd code. Overrides madc_opt_level for MIR gen.
extern thread_local bool madc_debug_info;
// True only while the production parser is building an isolated class pattern.
// DataDefs born in that scope retain speculative provenance after rollback.
extern thread_local bool madc_class_pattern_capture_active;

// Resolved absolute path of the running executable, empty when unresolvable.
// The one self-exe discovery point: readlink(/proc/self/exe) on Linux,
// _NSGetExecutablePath on macOS.
std::string madc_self_exe_path();

// Resolved absolute path of the IMAGE that contains libmadc's own code: the
// shared library when the CLI/host links libmadc dynamically, the executable
// itself in the monolithic shape (static libmadc). dladdr on a libmadc-resident
// symbol; empty when unresolvable. The one self-library discovery point — the
// forest discovery chain's library-image arm compares it against
// madc_self_exe_path() to know whether a distinct library carrier exists.
std::string madc_self_lib_path();

// Host shared-library suffix for dlopen soname synthesis (-l<name>) and
// native shared-artifact naming.
#ifdef __APPLE__
#define MADC_DSO_SUFFIX ".dylib"
#else
#define MADC_DSO_SUFFIX ".so"
#endif

// The native-emit TARGET is an Apple/Mach-O platform: either an emit-only
// cross madc configured for one (MADC_CROSS_APPLE) or a madc hosted on one
// (native target == host). Orthogonal to MADC_CROSS_TARGET, which gates the
// run lanes: a hosted-darwin madc is Apple-target but NOT cross. Lives here
// (not madc_cir.h) because the parser keys on it too (asm-label symbol space).
#if defined(MADC_CROSS_APPLE) || defined(__APPLE__)
#define MADC_TARGET_APPLE_P 1
#else
#define MADC_TARGET_APPLE_P 0
#endif

class TokenBase;

// C fixed-size array dimension. 64-bit: huge dims like
// `short buf[(1L << 62) - 256]` must survive layout/sizeof/offset math
// (gcc.c-torture largesizeofquery) — never store a dim in a 32-bit type.
typedef uint64_t carray_dim_t;

// The constant-fold carrier (P0 slice 3). The parse-time fold spine
// (parse_constant_*, resolve_integer_constant, case-label folding) computes at
// 128-bit precision, mirroring gcc's wide_int model: values carry more
// precision than their type; typed cast points truncate; int64 consumers
// truncate implicitly at the assignment boundary (which IS gcc's #if
// semantics — intmax_t is 64-bit). The carrier is signed; sub-64-bit
// signedness is handled where casts truncate (apply_integer_cast_value), not
// tracked per value — same discipline the int64 fold always used, one word up.
typedef __int128          madc_wide_int;
typedef unsigned __int128 madc_wide_uint;

enum class BaseType : uint8_t { btSimple, btStruct, btFunct, btClass,
				// An unresolved template parameter `T` (DataDefTemplateParam).
				// Append-only: never renumber. is_numeric/is_integer/is_real
				// gate on btSimple and is_struct/is_object/is_function on their
				// own basetypes, so a btTemplateParam answers false to all of
				// them without any per-predicate change.
				btTemplateParam };
enum class RefType  : uint8_t { rtNone, rtValue, rtPointer, rtReference  };

// Kind of a unary derived type, for the id-addressable derived-type API
// (Program::derived_type_id). Pointer / reference / const are the unary
// derivations madc memoizes today (getPointerType / getReferenceType /
// getConstType). Append-only: array / fn join when their creators are
// id-routed (no getArrayType exists yet). See
// docs/plans/2026-06-12-type-table-value-abi-design.md §2.
enum class DerivedKind : uint8_t { dkPointer, dkReference, dkConst };

enum class DataType : uint16_t {
	// Simple data types
	dtVOID, dtBOOL, dtUINT8, dtBYTE=dtUINT8,  dtINT8, dtCHAR = dtINT8,
	dtUINT16, dtINT16, dtSHORT=dtINT16, dtUINT24, dtINT24,
	dtUINT32, dtINT32, dtUINT64, dtINT64, dtINT=dtINT32,
	dtFLOAT=12, dtFLOAT32=dtFLOAT, dtDOUBLE, dtDOUBLE64=dtDOUBLE,
	dtLDOUBLE, dtDOUBLE80 = dtLDOUBLE, dtSIMD,
	// Append-only tail (PCH/typeid discipline): new simple types take the
	// next free slot; never renumber. 128-bit integers live ABOVE dtFLOAT,
	// so the type predicates (is_integer/is_real) use explicit sets, not
	// the historical "< dtFLOAT" range.
	dtINT128, dtUINT128, dtRESERVED = 255,
	dtMUTEX = 256, dtTHREAD, dtTHISTHREAD,
	dtARRAY,
		// Pointer/reference DERIVATION is no longer a numeric band on this
		// enum (the retired dt*ptr=+10000 / dt*ref=+20000 ranges). Derived
		// types ARE the structural object graph — DataDefPTR / DataDefREF /
		// DataDefCONST (with base_type) plus the typeid table. Ask
		// is_pointer() / is_reference() / base_type / Program::derived_type_id,
		// never tag arithmetic.
		// (tag-arithmetic retirement: docs/plans/2026-06-30-tag-arithmetic-retirement-plan.md)
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
			  // bit 65536 retired: reference-ness now lives in the type
			  // (DataDefREF / Variable::is_reference()) — first-class refs Phase 2
			  vfCONSTDECL  =131072, // `const`-DECLARED var (vfCONSTANT for write
			                        // enforcement) whose value is NOT set() into
			                        // data — so it must NOT be read-fold-substituted
			  vfCONSTBAKED =262144, // `const`-DECLARED var whose parse-time-known
			                        // initializer value WAS set() into data — an
			                        // integral constant expression; read-fold OK
			  vfLINKONCE   =524288, // C++ `inline` variable (vague linkage): every
			                        // including TU defines it — the CIR backend
			                        // emits a linkonce data binding (STB_WEAK) so
			                        // per-TU copies merge at a multi-.o link
			  vfINSTPRODUCT=1048576, // minted while a template was being
			                        // instantiated (_inst_depth > 0). A DERIVED
			                        // entity: bind time re-mints it from the
			                        // pattern, so it is never a header export
			                        // and never a lookup surface. Same rule
			                        // pack_tap_name applies to the decl index —
			                        // this is how the two stay consistent.
			                        // (Fresh bit: 65536 is RETIRED, and reusing
			                        // it would misread older serialized flags.)
			} varflag_t;

// The rt{None,Val,Ptr,Ref,DePtr,DeRef} tag-arithmetic macros are retired:
// pointer/reference derivation is the DataDefPTR/REF/CONST object graph, not
// a +/-10000/20000 offset on the DataType tag (tag-arithmetic retirement).

class DataDef
{
protected:
    uint32_t     _type;
public:
    std::string	 name;
    size_t	 size;
    // Canonical C++ type spelling for Itanium mangling, e.g.
    // "ns::Box<T>". Empty = use `name` (or a builtin spelling). Set from parsed
    // namespace/template declarations so a bodyless C++ method can bind to its
    // external mangled symbol without class-name tests.
    // Write-funneled: StructRegistry::find_despaced caches a pure function of
    // this spelling per struct_map entry, so every rewrite of an already-swept
    // dd must bump canonical_spelling_gen — set_canonical_spelling() is the
    // only write channel.
private:
    std::string	 canonical_cpp_spelling_;
public:
    // Despaced-canonical index invalidation counter (defined in parser.cpp).
    static uint64_t canonical_spelling_gen;
    // True once StructRegistry::find_despaced has swept this dd; a spelling
    // rewrite on an unswept dd needs no gen bump (the registry's size-stamp
    // top-up sees it fresh).
    bool	 canonical_swept;
    const std::string &canonical_cpp_spelling() const { return canonical_cpp_spelling_; }
    void set_canonical_spelling(const std::string &s)
    {
	if ( canonical_swept && canonical_cpp_spelling_ != s )
	    ++canonical_spelling_gen;
	canonical_cpp_spelling_ = s;
    }
    // Marshalling-boundary predicate (libmadc value kinds): true when this
    // type is the class that carries madc::value's TEXT kind, i.e. a value
    // of it marshals to kind::string at the libmadc boundary (runtime-eval
    // scope capture now; string call marshalling next). Defined in
    // src/madc_mangle.cpp — the one permitted home for std:: symbol
    // knowledge (scripts/check-no-std-hardcoding.sh); callers ask the
    // marshalling question and never name the type.
    bool marshals_value_text() const;
    // Itanium desugaring for a PLAIN SCALAR (a typedef alias dd like
    // std::streamoff, or a builtin scalar dd): the builtin C spelling for
    // its DataType ("long", "unsigned int", ...), or "" when this dd is not
    // a plain scalar (enum/class/pointer/complex/... keep their own
    // spelling). Itanium mangling encodes canonical types, never typedef
    // names. Defined in src/madc_mangle.cpp.
    std::string mangle_scalar_spelling() const;
    // Strict-equality (===) type-domain identity: do two types share one
    // value domain? Spec: docs/superpowers/specs/2026-06-11-strict-equality-design.md
    // §2.1. Defined in src/parser.cpp (needs the DataDef subclass set).
    bool same_representation(DataDef &d);
    // Canonical typeid: index into the segmented type table
    // (include/madc_typeid.h; docs/plans/2026-06-12-type-table-value-abi-design.md
    // §2). 0 = not yet registered. Primitives carry fixed ABI slots
    // (stamped by madc_stamp_primitive_type_ids()); everything else is
    // lazy-stamped into the active project table by madc_type_id_for()
    // (Program::type_id_for binds its own table and delegates).
    uint32_t	 type_id;
    // Intrinsic provenance for temporary semantic objects born while an
    // isolated class-pattern production parse is active.
    bool	 speculative_class_capture;
    DataDef()
	: _type(0), name(), size(0), canonical_cpp_spelling_(),
	  canonical_swept(false), type_id(0),
	  speculative_class_capture(madc_class_pattern_capture_active) {}
    DataDef(std::string n, size_t s, DataType d)
	: _type((uint32_t)d), name(n), size(s), canonical_cpp_spelling_(),
	  canonical_swept(false), type_id(0),
	  speculative_class_capture(madc_class_pattern_capture_active) {}
    DataDef(const DataDef &other)
	: _type(other._type), name(other.name), size(other.size),
	  canonical_cpp_spelling_(other.canonical_cpp_spelling_),
	  canonical_swept(other.canonical_swept), type_id(other.type_id),
	  speculative_class_capture(other.speculative_class_capture
	      || madc_class_pattern_capture_active) {}
    DataDef &operator=(const DataDef &other)
    {
	if ( this != &other )
	{
	    _type = other._type;
	    name = other.name;
	    size = other.size;
	    canonical_cpp_spelling_ = other.canonical_cpp_spelling_;
	    canonical_swept = other.canonical_swept;
	    type_id = other.type_id;
	}
	return *this;
    }
    virtual ~DataDef() {}
    virtual bool is_compatible(DataDef &d)
    {
	if ( &d == this
	||   rawtype() == d.rawtype()
	||  (is_numeric() && d.is_numeric()) )
	    return true;

	return false;
    }
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
	// 128-bit integers sit above dtFLOAT in the append-only enum tail.
	return _type == (uint16_t)DataType::dtINT128
	    || _type == (uint16_t)DataType::dtUINT128;
    }
    virtual bool is_real() const
    {
	if ( basetype() != BaseType::btSimple )
	    return false;
	// Explicit set: the enum tail past dtLDOUBLE holds non-real types
	// (dtSIMD, dtINT128, ...), so the historical [dtFLOAT, dtRESERVED)
	// range no longer classifies correctly.
	return _type == (uint16_t)DataType::dtFLOAT
	    || _type == (uint16_t)DataType::dtDOUBLE
	    || _type == (uint16_t)DataType::dtLDOUBLE;
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
	    case DataType::dtUINT128:
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
	// Structural: only DataDefPTR (and DataDefCONST forwarding to it) is a
	// pointer. The +10000 tag band is retired; a plain DataDef never is.
	return false;
    }
    // True only for DataDefREF: a type that came from a reference spelling
    // (`T&` / `T&&`, `typedef T& alias;`, `using alias = T&;`). Lowered as a
    // pointer, but the type keeps its reference-ness so consumers recover the
    // canonical T& — the single source of truth for reference identity (params,
    // returns, variables all carry it in the type; no parallel flags).
    virtual bool is_reference() const
    {
	return false;
    }
    // True only for DataDefCONST: a const-qualified type (`const T`). Lowered
    // identically to T (const has no runtime/ABI effect — same size, DataType,
    // codegen), but the type keeps its const-ness so type identity and spelling
    // carry `const` — the single source of truth for const, no parallel flags
    // (mirrors is_reference()/DataDefREF). See docs/plans/2026-06-19-const-qualified-types.md.
    virtual bool is_const() const
    {
	return false;
    }
    // True iff this is a C string: a pointer whose immediate pointee is a char
    // scalar (modulo const at either level) — char*, const char*, char* const,
    // char& (a reference lowers as the pointer). EXCLUDES char** and other
    // pointers. The STRUCTURAL replacement for the old `type() == dtCHARptr` tag
    // comparison (tag-arithmetic retirement). Non-virtual; defined out-of-line
    // below because it needs the complete DataDefPTR / DataDefCONST types.
    bool is_cstr() const;
    // True only for DataDefTemplateParam: an UNRESOLVED template parameter `T`
    // in a not-yet-instantiated template pattern (two-tree / materialize-from-AST
    // Phase 1.5). A real typed placeholder — NOT the bare TokenIdent the parser
    // substitutes at the token level today, and distinct from
    // DataDefCLASS::is_dependent_placeholder (which marks a dependent class
    // RESULT like `Box<T>`, not the param itself). tsubst replaces it with the
    // concrete argument type per instantiation; until then it must never reach
    // c2mir (the append_type_specs guard rejects it via an error node).
    virtual bool is_template_param() const
    {
	return false;
    }
    // True only for DataDefMemberPtr: a C++ pointer-to-member (`T C::*`).
    // Lowered as a scalar (a ptrdiff_t offset for a data member), but distinct
    // from an ordinary pointer so the `.*`/`->*` operators (Stage 2) and
    // overload resolution can recover the member-pointer-ness.
    virtual bool is_member_pointer() const
    {
	return false;
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
	switch ( rawtype() )
	{
	    case DataType::dtARRAY:
		return true;
	    default:
		return false;
	}
	return false;
    }
    // A madc `array` VALUE object (the script-level `array` = madc::value) —
    // not a pointer to one.
    bool is_madc_array() const
    {
	return rawtype() == DataType::dtARRAY && !is_pointer();
    }
    virtual bool has_ostream()
    {
	return false;
    }
    virtual bool has_istream()
    {
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
    // Strip pointer/reference derivation down to the underlying scalar tag.
    // A plain DataDef IS its scalar; DataDefPTR/REF override to forward to
    // base_type. (The old static band-subtraction overload and the +/-10000
    // setRef() are gone — derivation lives in the object graph now.)
    virtual DataType rawtype()  const
    {
	return (DataType)_type;
    }
    // Derivation kind. Structural: a plain DataDef is a value; DataDefPTR ->
    // rtPointer, DataDefREF -> rtReference, DataDefCONST forwards to base.
    virtual RefType reftype()  const
    {
	return RefType::rtValue;
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
class DataDefCLASS; // forward dec (member_vbase maps a member index to its virtual base)

enum class AggregateDefinitionOrigin : uint8_t
{
    Unknown,
    Included,
    TranslationUnitRoot
};

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
    std::vector<std::vector<carray_dim_t>> member_dims;
    std::vector<TokenBase *> member_count_exprs;	// runtime-sized member count expr, or NULL
    std::vector<uint32_t> member_access;	// per-member access flags (0=public, vfPRIVATE, vfPROTECTED)
    std::vector<int> member_origin;	// per-member: base index it came from, or -1 = own (MI flatten)
    struct AnonymousAggregateInfo
    {
	size_t first_member;
	size_t member_count;
	const DataDefSTRUCT *aggregate;
	size_t offset;
	AnonymousAggregateInfo()
	    : first_member(0), member_count(0), aggregate(NULL), offset(0) {}
	AnonymousAggregateInfo(size_t first, size_t count,
			       const DataDefSTRUCT *agg, size_t off)
	    : first_member(first), member_count(count), aggregate(agg),
	      offset(off) {}
    };
    std::vector<AnonymousAggregateInfo> anonymous_aggregates;
    // Member index -> the VIRTUAL base it belongs to (direct or transitive). A
    // shared virtual base is flattened ONCE (by vbase closure), and its members
    // resolve their final offset against vbase_offset[that base], NOT a per-path
    // direct-base offset. Absent = not a virtual-base member. Mirrors the
    // member_explicit_align map pattern (index-keyed, no parallel-vector burden).
    std::map<size_t, DataDefCLASS *> member_vbase;
    std::map<size_t,size_t> member_explicit_align; // member index -> __attribute__((aligned(N))); absent = natural
    // C++11 default member initializer (NSDMI): member NAME -> the PARSED init
    // expression (`int x = 5;` -> TokenInt(5)). Applied at default construction as
    // `__this->member = expr` for any member not explicitly initialized. Absent =
    // no in-class initializer (or one that did not parse — object members then take
    // the existing value-init construction). Name-keyed (survives MI reordering).
    std::map<std::string, TokenBase *> member_default_inits;
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
    AggregateDefinitionOrigin definition_origin;
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

    size_t field_storage_size(const DataDef &dd) const
    {
	const DataDefSTRUCT *s = dynamic_cast<const DataDefSTRUCT *>(&dd);
	if ( dd.size == 0 && s && s->is_complete
	  && dd.basetype() == BaseType::btClass )
	    return 1;
	return dd.size;
    }

    // The VIRTUAL base hosting the named member (member_vbase provenance), or
    // NULL for an own / non-virtual-base member.
    DataDefCLASS *member_vbase_host(const std::string &mname) const
    {
	for ( std::map<size_t, DataDefCLASS *>::const_iterator vi = member_vbase.begin();
	      vi != member_vbase.end(); ++vi )
	    if ( vi->first < members.size() && members[vi->first].first == mname )
		return vi->second;
	return NULL;
    }

//    DataDefSTRUCT(std::string n) : DataDef(n, 0, DataType::dtRESERVED) {}
    DataDefSTRUCT(std::string n, size_t s, DataType d=DataType::dtRESERVED)
	: DataDef(n, s, d), runtime_size_expr(NULL), pack(0), max_align(1), tag_explicit_align(0), union_layout(false),
	  is_complete(false), has_anon_aggregate(false),
	  reverse_scalar_storage(false), bitfield_active(false),
	  definition_origin(AggregateDefinitionOrigin::Unknown), bitfield_unit_offset(0),
	  bitfield_unit_size(0), bitfield_next_bit(0) {}
    DataDefSTRUCT(std::string n, std::vector<memberpair_t> m)
	: DataDef(n, 0, DataType::dtRESERVED), runtime_size_expr(NULL), pack(0), max_align(1), tag_explicit_align(0),
	  union_layout(false), is_complete(false), has_anon_aggregate(false),
	  reverse_scalar_storage(false), bitfield_active(false),
	  definition_origin(AggregateDefinitionOrigin::Unknown), bitfield_unit_offset(0),
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
	// Flip each field's bit_offset within its own window from the
	// PER-FIELD recorded state — fields of different declared types may
	// share a window (SysV packing, task #76), so replaying run
	// bookkeeping by (storage_offset, storage_size) transitions would
	// misplace shared-window fields.
	for ( size_t i = 0; i < member_bitfields.size(); ++i )
	{
	    BitFieldInfo &info = member_bitfields[i];
	    if ( !info.is_bitfield )
		continue;
	    size_t storage_bits = info.storage_size * 8;
	    size_t forward = info.reverse_storage
		? (storage_bits - info.bit_offset - info.bit_width)
		: info.bit_offset;
	    info.bit_offset = reverse
		? (storage_bits - forward - info.bit_width)
		: forward;
	    info.reverse_storage = reverse;
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
	bool is_array_decl = false, const std::vector<carray_dim_t> *dims = NULL)
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
	    member_dims.push_back(dims ? *dims : std::vector<carray_dim_t>());
	    member_count_exprs.push_back(count_expr);
	    member_access.push_back(0);
	    size_t member_size = count_expr ? 0 : (field_storage_size(dd) * cnt);
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
	member_dims.push_back(dims ? *dims : std::vector<carray_dim_t>());
	member_count_exprs.push_back(count_expr);
	member_access.push_back(0);
	if ( !count_expr )
	    size += field_storage_size(dd) * cnt;
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
	// SysV/gcc bitfield packing (task #76): a bitfield is placed at the
	// next free BIT; the only constraint is that it must not cross a
	// sizeof(T)-aligned window of its OWN declared type. Consecutive
	// bitfields of DIFFERENT types share bytes when they fit —
	// {_Bool b:1; unsigned x:5;} is ONE shared window (gcc/c2mir: 4
	// bytes), not two allocation units (was 8). The recorded
	// (storage_offset, storage_size, bit_offset) triple still names a
	// naturally-aligned window of the declared type containing the
	// field, so access consumers load/store exactly as before.
	size_t next_bit = bitfield_active
	    ? bitfield_unit_offset * 8 + bitfield_next_bit
	    : size * 8;
	if ( next_bit % storage_bits + width > storage_bits )
	    next_bit = align_up(next_bit, storage_bits);
	size_t window_offset = next_bit / storage_bits * storage_size;
	size_t fa = field_align(dd);
	if ( fa > max_align ) max_align = fa;
	bitfield_active = true;
	bitfield_unit_offset = window_offset;
	bitfield_unit_size = storage_size;
	bitfield_next_bit = next_bit - window_offset * 8;

	BitFieldInfo info;
	info.is_bitfield = true;
	info.storage_offset = window_offset;
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
	// Only the bytes the fields actually occupy count toward size —
	// finalize() and the next plain member's align_up supply padding.
	size_t end_byte = window_offset + (bitfield_next_bit + 7) / 8;
	if ( end_byte > size )
	    size = end_byte;
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
	member_dims.push_back(std::vector<carray_dim_t>());
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
	size_t first_member = members.size();
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
	    member_dims.push_back(i < agg.member_dims.size() ? agg.member_dims[i] : std::vector<carray_dim_t>());
	    member_count_exprs.push_back(i < agg.member_count_exprs.size() ? agg.member_count_exprs[i] : NULL);
	    member_access.push_back(i < agg.member_access.size() ? agg.member_access[i] : 0);
	    member_origin.push_back(i < agg.member_origin.size() ? agg.member_origin[i] : -1);
	    if ( agg.member_explicit_align.find(i) != agg.member_explicit_align.end() )
		member_explicit_align[members.size() - 1] = agg.member_explicit_align.at(i);
	}
	size_t end = base_offset + agg.size;
	if ( union_layout )
	{
	    if ( end > size ) size = end;
	}
	else
	    size = end;
	if ( agg.members.size() > 0 )
	    anonymous_aggregates.push_back(AnonymousAggregateInfo(
		first_member, agg.members.size(), &agg, base_offset));
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
    const std::vector<carray_dim_t> *m_dims(const std::string &member) const
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
    // A user `operator=` declared `= delete` is dropped from `methods` (like every
    // defaulted/deleted special member). Record the fact so __is_assignable can
    // report the class as NOT copy-assignable (a wrong "true" would corrupt SFINAE).
    bool has_deleted_copy_assign = false;
    // Same recording for the dropped SPECIAL-MEMBER CONSTRUCTORS, consumed by
    // __is_constructible: a deleted default/copy(move) ctor makes the class not
    // so-constructible; an explicitly-defaulted default ctor keeps the class
    // default-constructible even beside other user ctors ([class.default.ctor]).
    bool has_deleted_default_ctor = false;
    bool has_deleted_copy_ctor = false;
    bool has_defaulted_default_ctor = false;
    std::map<std::string, Variable *> method_map; // unmangled name -> method variable
    std::map<std::string, DataDef *> type_aliases; // class-scope typedef/using aliases
    std::vector<std::string> friend_class_names; // class names granted friend access
    std::vector<std::string> friend_function_names; // friend FUNCTION declarations
				// (name-based grant, like friend_class_names —
				// hidden-friend operators hoisted to namespace scope)
    std::map<std::string, DataDef *> static_member_types; // class-scope static data members
    // Integral static-const data members with a constant in-class initializer
    // (e.g. `static const bool value = __is_class(T);` — the std::integral_constant
    // pattern). Captured at parse so `X::value` / `X<T>::value` read the real value
    // instead of a 0 placeholder.
    std::map<std::string, int64_t> static_member_const_values;
    // Constructor overload set (each entry's FuncDef carries the param signature
    // and, for class-bound externals, an emit_symbol naming the real ctor).
    // A class with one ClassName() ctor has a single entry; a class with
    // overloaded ctors has several. Empty when the class has no recorded ctor.
    std::vector<Variable *> ctors;
    bool has_user_ctor;  // true if user defined ClassName() constructor
    bool has_user_dtor;  // true if user defined ~ClassName() destructor
    void *extern_ctor;   // C function pointer for extern class default constructor (NULL if none)
    void *extern_dtor;   // C function pointer for extern class destructor (NULL if none)
    void *_dtor_ptr;     // copy of extern_dtor; &_dtor_ptr is fn_indirect for cleanup stack
    DataDefCLASS *enclosing_class; // non-null for classes declared inside another class
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
    size_t class_align = 0; // TRUE class alignment (members + vptr + bases); set by compute_layout. 0 = not yet computed
    bool has_vptr_slot;   // class carries a vptr (virtual methods OR a virtual base); set by compute_layout
    // The class has a vptr to stamp and a vtable object to emit/read: either
    // polymorphic (has_vtable) or vbase-carrying without virtual functions
    // (has_vptr_slot only — Itanium still gives it a prologue-only vtable of
    // [vbase offsets, offset_to_top, RTTI] per group).
    bool has_any_vptr() const { return has_vtable || has_vptr_slot; }
    // A class's alignment is the strongest of its members, bases, and (if
    // polymorphic) the vptr — computed by compute_layout and cached in
    // class_align. Until then, fall back to the own-member alignment (max_align).
    virtual size_t alignment() const { return class_align ? class_align : DataDefSTRUCT::alignment(); }
    bool is_dependent_placeholder; // synthesized unresolved/dependent C++ type
    // Placeholder minted OUTSIDE a dependent (pattern) parse — a CONCRETE
    // forward tag (the empty-pack recursion tail _Tuple_impl<1>): live
    // registers it, inherits from it, and emits it without completing, so it
    // is legitimate frozen state; only pattern-context placeholders are
    // pack artifacts the freeze kills.
    bool opaque_concrete_tag;
    bool has_dependent_surface; // parsed class whose template args/bases still carry dependent lookup
    bool is_polymorphic() const { return has_vtable; }
    // True for a polymorphic class madc does NOT define: every owned method,
    // ctor and dtor is a bodyless external declaration (bound to a libstdc++
    // Itanium symbol), and its whole base chain is likewise external. madc emits
    // NONE of its machinery (vtable, typeinfo, implicit ctor/dtor) and instead
    // references the real _ZTVSt.../_ZTISt... symbols where the class is used.
    // Purely data-driven (declaration_only/emit_symbol aggregation) — never a
    // namespace or class-name test. Defined in parser.cpp (needs FuncDef).
    bool is_externally_defined() const;
    void compute_layout(); // Itanium layout engine (defined in parser.cpp)
    void apply_member_layout(); // rewrite member_offsets from member_origin + computed layout
    // Subobject offset of `target` within this class (direct/transitive base), or
    // (size_t)-1 if not a base. Used to adjust `this` on base-method calls.
    size_t base_offset_of(const DataDefCLASS *target) const;
    // RTTI (S5b): which Itanium type_info flavor this class needs.
    //   TI_CLASS = no bases; TI_SI = exactly one public non-virtual base;
    //   TI_VMI = multiple bases, or any virtual / non-public base.
    enum TypeInfoFlavor { TI_CLASS, TI_SI, TI_VMI };
    TypeInfoFlavor typeinfo_flavor() const;
    // True iff `b` is reachable as a UNIQUE public non-virtual base; if so writes
    // its subobject offset to *off. Drives the __si flavor choice and the
    // dynamic_cast src2dst hint.
    bool is_unique_public_nonvirtual_base(DataDefCLASS *b, size_t *off) const;
    // Collect all (transitive) virtual bases, deduped, in canonical order.
    void collect_vbases(std::vector<DataDefCLASS *> &out,
			std::set<DataDefCLASS *> &seen) const;
    // Virtual function table
    std::vector<std::string> vtable_slots; // method names in vtable slot order
    std::map<std::string, bool> virtual_methods;  // names of methods declared virtual
    void **vtable;         // runtime vtable (array of function pointers, filled at compile time)
    bool has_vtable;       // true if this class or any base has virtual methods
    bool from_system_header;  // defined in a system/toolchain header (glibc/libstdc++):
			      // the library owns its vtable/typeinfo/member symbols (explicit
			      // instantiation), so madc defers even when the header gives a
			      // virtual slot an inline default. Data-driven (path-based).
    bool is_extern_template_instantiated; // an `extern template class X<...>;`
			      // explicit-instantiation DECLARATION named this exact
			      // instantiation: libstdc++ exports ALL its members out-of-line
			      // (C1/D1/methods), so madc binds them to the real mangled
			      // symbols instead of emitting bodies — even for a NON-polymorphic
			      // class that is_externally_defined() (which requires a vtable)
			      // does not cover. The data-driven signal distinguishes exported
			      // template instantiations from inline-only local instantiations
			      // — never a name test.
    int vtable_slot(const std::string &name) const {
	for ( size_t i = 0; i < vtable_slots.size(); ++i )
	    if ( vtable_slots[i] == name ) return (int)i;
	return -1;
    }
    // Grouped vtable (Itanium MI): one sub-table per polymorphic subobject. Group 0
    // is the primary (this_offset 0, shares the offset-0 vptr); each subsequent group
    // is a secondary polymorphic base at its subobject offset (its own __vptr_<off>).
    // addr_point = the slot's starting index in the emitted flat Cls__vtable[] array.
    struct VtableGroup {
	DataDefCLASS *owner;
	size_t this_offset;
	std::vector<std::string> slots;
	size_t addr_point;
    };
    std::vector<VtableGroup> vtable_groups;
    void build_vtable_groups(); // defined in parser.cpp; run after compute_layout
    // Resolve a virtual method name to its (group, in-group slot). Returns false if
    // not a virtual method of any group.
    bool find_vslot(const std::string &m, size_t &group, int &slot) const {
	for ( size_t g = 0; g < vtable_groups.size(); g++ )
	    for ( size_t i = 0; i < vtable_groups[g].slots.size(); i++ )
		if ( vtable_groups[g].slots[i] == m ) { group = g; slot = (int)i; return true; }
	return false;
    }
    bool is_virtual_method(const std::string &name) const {
	if ( virtual_methods.find(name) != virtual_methods.end() ) return true;
	if ( base_class ) return base_class->is_virtual_method(name);
	return false;
    }
    // Is this class the same as `target`, or derived (directly or transitively,
    // through any base) from it? Walks the full multiple/virtual base graph. Used
    // for protected-member access and derived->base pointer upcasts (the subobject
    // offset is base_offset_of(target)).
    bool is_or_derives_from(const DataDefCLASS *target) const {
	if ( this == target ) return true;
	for ( const auto &b : bases )
	    if ( b.base == target || b.base->is_or_derives_from(target) ) return true;
	return false;
    }

    DataDefCLASS(std::string n, size_t s, DataType d)
	: DataDefSTRUCT(n, s, d), has_user_ctor(false), has_user_dtor(false),
	  extern_ctor(NULL), extern_dtor(NULL), _dtor_ptr(NULL),
	  enclosing_class(NULL), base_class(NULL), nvsize(0), own_block_off(0),
	  has_vptr_slot(false), is_dependent_placeholder(false),
	  opaque_concrete_tag(false),
	  has_dependent_surface(false), vtable(NULL), has_vtable(false),
	  from_system_header(false), is_extern_template_instantiated(false) {}
    virtual BaseType basetype() const { return BaseType::btClass; }
    Variable *findMethod(const std::string &s);
    // Among the same-name method overloads (this class + base chain), pick the
    // one whose parameter types best match `argtypes` (overload resolution by
    // argument type). Returns NULL when no same-name method exists; falls back
    // to the first by-name match when none scores strictly better (e.g. a
    // single, non-overloaded method — same result as findMethod).
    Variable *findMethodOverload(const std::string &name,
				 const std::vector<const DataDef *> &argtypes);
    // Return type of the BINARY operator method `opname` (e.g. "operator+") this
    // class declares; used to type a class-object operator expression with the
    // operator's declared result type.
    // Prefers a parameterized (binary) overload; searches the unmangled name then
    // the mangled ClassName__operatorX family, then the base chain. NULL if none.
    DataDef *binary_operator_return_type(const std::string &opname);
    // True iff this class declares at least one binary `opname` member AND every
    // such member's explicit parameter is a NON-class (arithmetic/pointer) type —
    // i.e. no member can bind a class-object rhs. The iterator signature
    // (`operator-(difference_type)` only) where `iter - iter` must instead bind
    // the free cross-type operator template. Members taking a class parameter
    // make this false (they own the expression by normal overload rules).
    bool binary_operator_only_takes_nonclass(const std::string &opname);
    // Return type of a unary operator method (`operator-`, `operator!`,
    // `operator++`, etc.). `postfix` selects the parameterized postfix form for
    // ++/-- and the nullary form otherwise.
    DataDef *unary_operator_return_type(const std::string &opname, bool postfix);
    void register_extern_ctor_dtor(void *ctor, void *dtor) {
	extern_ctor = ctor; extern_dtor = dtor; _dtor_ptr = dtor;
    }
};

typedef DataDefCLASS DDClass;

// data definitions of default base types
class DataDefVOID:      public DataDef { public: DataDefVOID() :   DataDef("void", 0,     DataType::dtVOID) {} };
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
// 128-bit integers: SysV x86-64 ABI alignment is 16 (the base alignment()
// caps simple types at 8, which is correct for every other scalar).
class DataDefINT128:    public DataDef { public:
	DataDefINT128():  DataDef("__int128", 16, DataType::dtINT128) {}
	virtual size_t alignment() const { return 16; } };
class DataDefUINT128:   public DataDef { public:
	DataDefUINT128(): DataDef("unsigned __int128", 16, DataType::dtUINT128) {}
	virtual size_t alignment() const { return 16; } };
class DataDefFLOAT:     public DataDef { public: DataDefFLOAT() :  DataDef("float", 4,    DataType::dtFLOAT) {} };
class DataDefDOUBLE:    public DataDef { public: DataDefDOUBLE():  DataDef("double", 8,   DataType::dtDOUBLE) {} };
// x86-64 SysV: the x87 80-bit extended type, 10 significant bytes but sizeof 16
// and 16-byte aligned — which is what both canons report and what the generated
// macro table (__LDBL_MAX__ 1.189e+4932) has always advertised. `long double`
// used to lex straight to ddDOUBLE, so sizeof said 8, printf("%Lg") read 80 bits
// off the varargs stack and printed nan, and the mangler emitted Itanium `e`
// for a value passed as a double.
class DataDefLDOUBLE:   public DataDef { public: DataDefLDOUBLE(): DataDef("long double", 16, DataType::dtLDOUBLE) {} };

// generic pointer-to-type — tracks what the pointer points to
// pointers are 64-bit integers at the ABI level (stored in Gp registers)
class DataDefPTR : public DataDef
{
public:
    DataDef *base_type;  // what this pointer points to
    DataDefPTR(DataDef &base)
	: DataDef(base.name + "*", 8, base.type()), base_type(&base) {}
    virtual bool is_pointer() const { return true; }
    virtual bool is_numeric() const { return true; }
    virtual bool is_integer() const { return true; }
    // Classify STRUCTURALLY, not from the _type tag-band (mirrors DataDefCONST,
    // which already forwards these to base_type). A pointer's reftype is
    // rtPointer by construction; its rawtype is the pointee's rawtype (so T**
    // recurses to the innermost scalar, matching the historical one-subtraction
    // tag behaviour). Part of retiring the +10000/+20000 tag encoding: the
    // structural object stops depending on the offset math
    // (docs/plans/2026-06-30-tag-arithmetic-retirement-plan.md).
    virtual RefType reftype() const { return RefType::rtPointer; }
    virtual DataType rawtype() const { return base_type ? base_type->rawtype() : DataType::dtVOID; }
};

// LPSTR is `char *`. It IS-A DataDefPTR(ddCHAR) — a structural pointer with a
// real base_type — not a plain DataDef carrying a bare dtCHARptr tag (that was a
// member of the plain-tag population the tag-arithmetic retirement is removing).
// Keeps its own object identity + name + pinned typeid slot (MADC_TYPEID_LPSTR);
// the ctor is out-of-line (parser.cpp) because the ddCHAR instance is declared
// later in this header. Constructed after ddCHAR (parser.cpp init order).
class DataDefLPSTR : public DataDefPTR { public: DataDefLPSTR(); };

// A reference type produced by alias resolution (`typedef T& alias;` /
// `using alias = T&;`). An alias is
// a type, not a spelling: the resolved type must carry the reference
// qualifier itself so it survives alias-chain hops. IS-A DataDefPTR (same
// name, size, DataType) because madc lowers T& as T*; everything that does
// not explicitly test is_reference() treats it exactly like the pointer it
// lowers to. base_type is the referee.
class DataDefREF : public DataDefPTR
{
public:
    DataDefREF(DataDef &base) : DataDefPTR(base) {}
    virtual bool is_reference() const { return true; }
    // A reference's reftype is rtReference — STRUCTURAL, overriding DataDefPTR's
    // rtPointer. (Historically a DataDefREF's _type sat in the POINTER band
    // because the ctor chains through DataDefPTR/rtPtr, so the inherited tag
    // reftype() reported rtPointer while is_reference() said true — the "three
    // encodings" disagreement. This override makes the two agree. rawtype()
    // is inherited from DataDefPTR (base_type->rawtype()), unchanged.)
    virtual RefType reftype() const { return RefType::rtReference; }
};

// `void&` — the reference-slot placeholder for MADC_TYPEID_VOID_REF. IS-A
// DataDefREF(ddVOID): was the LAST plain-tag global (a bare dtVOIDref tag) the
// tag-arithmetic retirement removes. Out-of-line ctor (parser.cpp) because
// ddVOID is declared later. is_reference() is now true, so same_representation
// strips it like any reference.
class DataDefVOIDref: public DataDefREF { public: DataDefVOIDref(); };

// A const-qualified type (`const T`). IS-A its base's lowering: const has NO
// runtime/ABI effect, so the size, DataType, and all codegen behaviour are the
// base's — but is_const() is true so type identity and the rendered name carry
// `const`, surviving deduction / template-instantiation keying (the missing
// identity behind map<int,int>'s pair-piecewise-ctor signature mismatch). Every
// predicate that does not test is_const() FORWARDS to the base, so a consumer
// that does not care about const treats a DataDefCONST exactly like its base
// (the DataDefREF discipline). base_type is the unqualified T.
// See docs/plans/2026-06-19-const-qualified-types.md (Phase 1 = this class).
class DataDefCONST : public DataDef
{
public:
    DataDef *base_type;
    DataDefCONST(DataDef &base)
	: DataDef("const " + base.name, base.size, base.type()), base_type(&base) {}
    virtual BaseType basetype() const { return base_type->basetype(); }
    virtual DataType rawtype() const { return base_type->rawtype(); }
    virtual RefType reftype() const { return base_type->reftype(); }
    virtual bool is_const() const { return true; }
    virtual bool is_complex() const { return base_type->is_complex(); }
    virtual bool is_pointer() const { return base_type->is_pointer(); }
    virtual bool is_reference() const { return base_type->is_reference(); }
    virtual bool is_member_pointer() const { return base_type->is_member_pointer(); }
    virtual bool is_struct() const { return base_type->is_struct(); }
    virtual bool is_object() const { return base_type->is_object(); }
    virtual bool is_function() const { return base_type->is_function(); }
    virtual bool is_numeric() const { return base_type->is_numeric(); }
    virtual bool is_integer() const { return base_type->is_integer(); }
    virtual bool is_real() const { return base_type->is_real(); }
    virtual bool is_simd() const { return base_type->is_simd(); }
    virtual bool is_unsigned() const { return base_type->is_unsigned(); }
    virtual size_t alignment() const { return base_type->alignment(); }
};

// is_cstr() — declared in DataDef above; defined here where DataDefPTR /
// DataDefCONST are complete. Strip a top-level const (char* const), require a
// pointer, then require the immediate pointee to be a char scalar (its rawtype
// unwraps an inner const for const char*; !is_pointer() excludes char**).
// Value-equivalent to the historical `type() == dtCHARptr` for every type, and
// independent of the +10000 tag (so it survives the tag's removal).
inline bool DataDef::is_cstr() const
{
    const DataDef *d = this;
    if ( const DataDefCONST *cd = dynamic_cast<const DataDefCONST *>(d) )
	d = cd->base_type ? cd->base_type : d;
    const DataDefPTR *p = dynamic_cast<const DataDefPTR *>(d);
    if ( !p || !p->base_type )
	return false;
    return p->base_type->rawtype() == DataType::dtCHAR
	&& !p->base_type->is_pointer();
}

// C++ pointer-to-DATA-member `T C::*`. Lowered (Itanium ABI) as a `ptrdiff_t`
// byte offset into the object — 8 bytes, an integer scalar for codegen — so it
// is a `dtINT64`-typed DataDef. Kept as its own class (NOT a plain integer) so
// `is_member_pointer()` is true and the `.*`/`->*` operators + overload
// resolution (Stage 2) can recover the owning class and member type. The null
// member pointer is -1 (a 0 offset is a valid first member). `owner_class` is the
// `C`; `member_type` is the pointee `T`. Member-FUNCTION pointers (a 16-byte
// `{ptr, this-adjust}` struct) are a separate, later representation.
class DataDefMemberPtr : public DataDef
{
public:
    DataDef *owner_class;        // the `C` in `T C::*` (NULL if unresolved at parse)
    std::string owner_name;      // the spelled owner (e.g. a nested-in-template class)
    DataDef *member_type;        // the pointee/member type `T`
    DataDefMemberPtr(DataDef *owner, const std::string &owner_nm, DataDef &member)
	: DataDef(member.name + " " + owner_nm + "::*", 8, DataType::dtINT64),
	  owner_class(owner), owner_name(owner_nm), member_type(&member) {}
    virtual bool is_numeric() const { return true; }
    virtual bool is_integer() const { return true; }
    virtual bool is_member_pointer() const { return true; }
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

    // A runtime dim ANYWHERE in the chain makes the whole array type
    // variably modified (C11 6.7.6): its size is a runtime value even when
    // the head dim is constant (`char a[3][n]`).
    bool chain_has_runtime_size() const
    {
	for ( const DataDefCArray *c = this; c;
	      c = dynamic_cast<const DataDefCArray *>(c->element_type) )
	    if ( c->has_runtime_size() )
		return true;
	return false;
    }
};

class DataDefENUM : public DataDef
{
public:
    std::string enum_name;
    // The enumeration's underlying type ([dcl.enum]): the declared fixed
    // base (`enum E : short`) when present, else computed from the
    // enumerator range at the definition's close (the canon g++/clang
    // rule: non-negative -> unsigned int, negatives -> int, wider values
    // -> the 64-bit twin; scoped unfixed -> int). NULL only for an opaque
    // declaration — readers fall back to int. Serves __underlying_type,
    // which both libstdc++'s and libc++'s std::underlying_type are built
    // on. NOTE the enum's own storage still LOWERS to int (I2); a fixed
    // base narrower than int changes only what __underlying_type answers,
    // not (yet) the enum's layout.
    DataDef *underlying = NULL;

    DataDefENUM(const std::string &name)
	: DataDef(name, sizeof(int), DataType::dtINT), enum_name(name) {}
};

class DataDefCOMPLEX : public DataDefSTRUCT
{
public:
    DataDef *element_type;

    // Floating-element complex is NATIVE (c2mir/MIR carry _Complex float/
    // double/long double); integer-element complex (GNU extension) is LOWERED
    // by the CIR builder to this struct{__re,__im} shape (SysV ABI of
    // integer _Complex == struct{T,T}). The lowered form is emitted as a real
    // struct definition, so its tag must be a clean C identifier; the native
    // form is never emitted by name.
    static std::string type_name(DataDef &elem)
    {
	if ( elem.is_real() )
	    return elem.name + " _Complex";
	std::string tag = "__madc_complex_" + elem.name;
	for ( size_t i = 0; i < tag.size(); ++i )
	    if ( tag[i] == ' ' )
		tag[i] = '_';
	return tag;
    }

    DataDefCOMPLEX(DataDef &elem)
	: DataDefSTRUCT(type_name(elem), 0), element_type(&elem)
    {
	// Member names must not be `__real`/`__imag` — those are gcc/clang
	// KEYWORDS, and the lowered form's struct def is emitted into C.
	addMember("__re", elem, 1);
	addMember("__im", elem, 1);
	// Only the LOWERED (integer-element) form is a real emittable struct;
	// marking the native form complete would make the struct-dep walkers
	// emit a bogus `struct double _Complex` definition.
	is_complete = !is_native();
    }

    virtual bool is_complex() const { return true; }
    // True: c2mir represents the type natively (TP_CFLOAT/TP_CDOUBLE/
    // TP_CLDOUBLE). False: integer element — the CIR builder lowers to the
    // struct spine (c2mir rejects integer _Complex).
    bool is_native() const { return element_type && element_type->is_real(); }

    virtual bool is_compatible(DataDef &d)
    {
	if ( &d == this )
	    return true;
	DataDefCOMPLEX *other = dynamic_cast<DataDefCOMPLEX *>(&d);
	return other && other->element_type == element_type;
    }

    size_t component_offset(bool imag_part) const
    {
	std::string member = imag_part ? "__im" : "__re";
	return const_cast<DataDefCOMPLEX *>(this)->m_offset(member);
    }
};

// The script `array` / `madc::array` builtin IS the public `madc::value`
// (include/libmadc/value.h) — one value type end-to-end. Display name stays
// "array" (Program::resolve_named_datadef in parser.cpp,
// builtin_datadef_from_spelling in pch.cpp, and the madc_ns alias keep
// resolving it); the canonical C++ identity used for Itanium mangling is
// `madc::value`, so script calls taking `array&` bind to the host
// `madc::value&` symbols. Keyed (context) data is kind::object; indexed
// (php-style) arrays are kind::array.
class DataDefARRAY: public DDClass
{
public:
    DataDefARRAY(): DDClass("array", sizeof(madc::value), DataType::dtARRAY)
    {
	set_canonical_spelling("madc::value");
    }
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
extern DataDefINT128 ddINT128;
extern DataDefUINT128 ddUINT128;
extern DataDefFLOAT ddFLOAT;
extern DataDefDOUBLE ddDOUBLE;
extern DataDefLDOUBLE ddLDOUBLE;
extern DataDefSTRUCT ddMAX_ALIGN_T;
extern DataDefLPSTR ddLPSTR;

extern DataDefPTR ddVOIDptr, ddCHARptr, ddINTptr, ddINT32ptr;
extern DataDefARRAY ddARRAY;


// auto type placeholder
class DataDefAUTO: public DataDef
{
public:
    DataDefAUTO() : DataDef("auto", 0, DataType::dtVOID) {}
};
extern DataDefAUTO ddAUTO;

// An UNRESOLVED template type parameter `T` in a not-yet-instantiated template
// pattern — the typed placeholder the two-tree / materialize-from-AST design
// needs (docs/plans/2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md, Phase
// 1.5). Today madc has no such type: a template body's `T` is a bare
// TokenIdent that the parser substitutes for a concrete TokenDataType BEFORE
// parsing, so a parsed pattern can never contain `T`. Representing `T` as a real
// type is the prerequisite for parsing a body ONCE into an immutable Tree-1
// pattern and instantiating by tsubst (copy + substitute) instead of re-parsing.
//
// It carries the spelled name (DataDef::name, e.g. "T") and param_index — the
// 0-based position in the template's parameter list — which tsubst uses to pick
// the concrete argument to substitute. Size 0 and DataType dtVOID: it has no
// real layout. basetype() is btTemplateParam so every inherited is_*() predicate
// answers false (it is not numeric/integer/real/pointer/struct/object/function);
// is_template_param() is the single discriminator consumers test (the
// DataDefREF/DataDefCONST discipline — identity lives in the type, no parallel
// flags). A pattern containing one is Tree-1 only and must be substituted before
// it can be lowered/compiled; append_type_specs rejects a stray one via an error
// node so an un-substituted placeholder surfaces loudly rather than mis-lowering.
class DataDefTemplateParam: public DataDef
{
public:
    unsigned param_index;   // 0-based position in the template parameter list
    DataDefTemplateParam(const std::string &nm, unsigned idx)
	: DataDef(nm, 0, DataType::dtVOID), param_index(idx) {}
    virtual BaseType basetype() const { return BaseType::btTemplateParam; }
    virtual bool is_template_param() const { return true; }
};

// Type table identity layer — slot <-> global-primitive mapping (the single
// source of truth; defined in src/parser.cpp next to same_representation).
DataDef *madc_primitive_for_slot(uint32_t slot);
void madc_stamp_primitive_type_ids();

// The typeid policy chokepoints (defined in src/parser.cpp): the dd->type_id
// lazy-stamp memo plus the primitive/system/project segment dispatch. These
// free functions are the ONE implementation; Program::type_id_for /
// type_from_id bind the active table and delegate here. Statically reachable
// so serializable-reference accessors (cir_node::datadef()) resolve typeids
// without a Program in hand — the same active-substrate discipline as
// TokenBase::_active_strpool. `madc_active_project_types` is bound by
// _parser_init (and by the Program methods on each call).
namespace madc { namespace dis { template<class T> class id_table; } }
extern madc::dis::id_table<DataDef> *madc_active_project_types;
uint32_t madc_type_id_for(DataDef *dd);
DataDef *madc_type_from_id(uint32_t id);

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
