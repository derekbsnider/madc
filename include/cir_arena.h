#ifndef __CIR_ARENA_H
#define __CIR_ARENA_H 1

// cir_arena.h — B3 arena-native DataDef storage (step-2 skeleton).
//
// The forest campaign's endgame (docs/plans/2026-07-06-forest-arena-native-scoping.md
// + 2026-07-06-forest-b3-record-layout-DESIGN.md): a DataDef's COMPLETE state lives as a
// flat POD record in one contiguous arena — cross-references are INDICES (not heap
// pointers), identifiers are intern-pool offsets, and variable-length collections are
// (begin,count) slices into a side payload. SAVE = dump the three blocks; LOAD = read them
// back with ZERO pointer fixup (everything is already index/offset relative). The live
// DataDef eventually becomes a thin HANDLE over a `defrec`; this header is the storage that
// handle wraps.
//
// It is built ON the existing substrate (madc::dis::intern_table + pod_record) — NOT a
// parallel arena. The forest's cir_forest_type_record family is the same idea as a
// serialization mirror; B3 promotes this to canonical live storage.
//
// PAYLOAD DISCIPLINE (load-bearing): every variable-length run (members, bases, methods,
// vbase pairs, vtable-group recs, vtable-group SLOT id runs, func params) is packed into the
// ONE `payload` block. A run must be CONTIGUOUS, so an encoder MUST resolve every child def
// index FIRST (the recursive encode of a member/param/base TYPE may itself append payload)
// and only THEN append its own run. Capturing begin=payload.size() before a loop that
// recurses would interleave a nested aggregate's run with this one.
//
// Inert until the live-handle migration wires it in (guarded by FEATURE_FOREST_ARENA at the
// call sites); only test_cir_arena.cpp exercises it today.

#include <cstdint>
#include <vector>
#include <string>

#include "madcdis/intern_table.h"
#include "madcdis/pod_record.h"

namespace madc {
namespace dis {

// Discriminant for a defrec — replaces the vtable for STORAGE purposes (the live handle
// still dispatches virtually; the tag drives (de)serialization + handle construction).
enum DefKind : uint32_t {
	DK_NONE = 0,
	DK_PRIM,	// a primitive scalar (int/char/float/... — name+size+datatype say all)
	DK_VOID,
	DK_PTR,		// ref0 = pointee index
	DK_REF,		// ref0 = referee index
	DK_CONST,	// ref0 = unqualified index
	DK_ENUM,
	DK_STRUCT,	// members_* slice
	DK_UNION,	// members_* slice, union layout
	DK_CLASS,	// members_* + bases_* + methods_* + vbase_* + vgroup_* (the flattenings)
	DK_FUNC,	// FuncDef: ref0 = return index, params_* slice
	DK_VAR,		// Variable: ref0 = type index (later increment)
	DK_SIMD,
	DK_FPTR,
	DK_MEMBERPTR,
	DK_CARRAY,
};

// Kind-independent flag bits on a defrec (grows as the schema completes — a new bool is a
// new bit, dumped for free).
enum DefFlags : uint32_t {
	DF_UNION_LAYOUT      = 1u << 0,
	DF_IS_COMPLETE       = 1u << 1,
	DF_IS_ANONYMOUS      = 1u << 2,
	DF_REVERSE_SCALAR    = 1u << 3,
	DF_HAS_ANON_AGG      = 1u << 4,
	// class:
	DF_HAS_VTABLE        = 1u << 5,
	DF_HAS_VPTR_SLOT     = 1u << 6,
	DF_FROM_SYSTEM_HDR   = 1u << 7,
	DF_HAS_USER_CTOR     = 1u << 8,
	DF_HAS_USER_DTOR     = 1u << 9,
	// func:
	DF_IS_VARARGS        = 1u << 10,
	DF_IS_VOID_PARAMS    = 1u << 11,
	DF_DECLARATION_ONLY  = 1u << 12,
};

// Per-base flag bits (baserec.flags).
enum BaseFlags : uint32_t {
	BSF_VIRTUAL = 1u << 0,
	BSF_PRIMARY = 1u << 1,
	// BaseSpec::access rides bits 8+ (the vf* flags fit in a byte).
	BSF_ACCESS_SHIFT = 8,
};

// One fixed-stride POD record per DataDef. uint32 words only (pod_record requires whole
// words + trivial copyability). Cross-refs are defrec INDICES; strings are intern ids;
// collections are (begin_word, count) slices into the arena payload. Unused fields are 0.
struct defrec {
	uint32_t kind;		// DefKind
	uint32_t name_id;	// intern id of DataDef::name
	uint32_t canon_id;	// intern id of canonical_cpp_spelling (0 = none)
	uint32_t size;		// DataDef::size
	uint32_t datatype;	// the originating DataType enum value (rawtype seed)
	uint32_t flags;		// DefFlags
	uint32_t ns_id;		// defining-namespace intern id (0 = global)
	uint32_t ref0;		// PTR/REF/CONST: operand index; FUNC: return index; VAR: type index; else 0
	// struct/class members:
	uint32_t members_begin;	// WORD offset of the memberrec run in payload
	uint32_t members_count;
	// struct layout scalars:
	uint32_t pack;
	uint32_t max_align;
	uint32_t tag_explicit_align;
	// class bases / methods:
	uint32_t bases_begin;	// baserec run
	uint32_t bases_count;
	uint32_t methods_begin;	// methodrec run
	uint32_t methods_count;
	// class virtual-base offsets (flattened map<DataDefCLASS*,size_t>):
	uint32_t vbase_begin;	// vbaserec run (sorted by class_idx)
	uint32_t vbase_count;
	// class vtable groups (flattened nested vector):
	uint32_t vgroup_begin;	// vgrouprec run
	uint32_t vgroup_count;
	// class layout scalars:
	uint32_t nvsize;
	uint32_t class_align;
	uint32_t own_block_off;
	// func params:
	uint32_t params_begin;	// paramrec run
	uint32_t params_count;
};

// One member of a STRUCT/CLASS (bitfield/vbase-index fields fold in as the schema
// completes). uint32 words only.
struct memberrec {
	uint32_t name_id;	// memberpair_t.first
	uint32_t type_idx;	// memberpair_t.second, as a defrec index
	uint32_t typedef_id;	// memberpair_t.typedef_name (0 = none)
	uint32_t offset;	// member_offsets[i]
	uint32_t count;		// member_counts[i]
	uint32_t flags;		// bit0 = array_flag; access in bits 1-2; (grows)
};

// A direct base (DataDefCLASS::bases -> BaseSpec).
struct baserec {
	uint32_t base_idx;	// BaseSpec.base, as a defrec index
	uint32_t offset;	// BaseSpec.offset
	uint32_t flags;		// BaseFlags (is_virtual | is_primary | access<<8)
};

// A method (DataDefCLASS::methods -> Variable* wrapping a FuncDef*).
struct methodrec {
	uint32_t name_id;	// Variable::name (the mangled call symbol)
	uint32_t func_idx;	// the method's FuncDef, as a defrec index (DK_FUNC)
	uint32_t flags;		// reserved (const/static/virtual — grows)
};

// A flattened virtual-base offset (the pointer-KEYED map, sorted by class_idx).
struct vbaserec {
	uint32_t class_idx;	// the virtual base, as a defrec index
	uint32_t offset;	// its subobject offset
};

// A vtable group (DataDefCLASS::VtableGroup): the nested `slots` vector is a SEPARATE
// (slots_begin, slots_count) run of raw intern ids in the payload.
struct vgrouprec {
	uint32_t owner_idx;	// VtableGroup.owner, as a defrec index
	uint32_t this_offset;	// VtableGroup.this_offset
	uint32_t slots_begin;	// WORD offset of the slot-id run (uint32 name_ids)
	uint32_t slots_count;	// number of slot ids
	uint32_t addr_point;	// VtableGroup.addr_point
};

// A function parameter (FuncDef::parameters[i]).
struct paramrec {
	uint32_t type_idx;	// the parameter type, as a defrec index
	uint32_t flags;		// bit0 = const_param (grows)
	uint32_t cpp_spelling_id;	// param_cpp_spellings[i] (0 = render from type)
};

// The arena: three self-contained, index/offset-addressed blocks. A byte dump is the three
// blocks concatenated; a load reads them back with no fixup (this is why round-tripping is a
// plain copy of the vectors — no live pointer ever enters the arena).
class DefArena {
public:
	intern_table          strings;	// every identifier / spelling
	std::vector<uint32_t> defs;	// defrec[] packed back-to-back by pod_append
	std::vector<uint32_t> payload;	// memberrec/baserec/methodrec/vbaserec/vgrouprec/paramrec + slot-id runs

	static uint32_t def_stride() { return (uint32_t)pod_words<defrec>(); }
	uint32_t def_count() const { return (uint32_t)(defs.size() / def_stride()); }

	// Append a defrec; return its DEF INDEX (not the word offset).
	uint32_t add_def(const defrec &r) {
		uint32_t woff = pod_append(defs, r);
		return woff / def_stride();
	}
	bool get_def(uint32_t idx, defrec &out) const {
		return pod_read(defs, (size_t)idx * def_stride(), out);
	}
	// Overwrite an already-appended defrec in place (two-phase build: reserve a slot, then
	// rewrite ref/slice fields once children/runs are known). idx must be < def_count().
	bool set_def(uint32_t idx, const defrec &r) {
		size_t base = (size_t)idx * def_stride();
		if ( base + def_stride() > defs.size() ) return false;
		const uint32_t *w = (const uint32_t *)&r;
		for ( uint32_t k = 0; k < def_stride(); ++k ) defs[base + k] = w[k];
		return true;
	}

	// Generic run helpers: append a POD record to payload (returns its word offset), and
	// read the i-th record of a run beginning at `begin`.
	template <typename T> uint32_t add_payload(const T &rec) { return pod_append(payload, rec); }
	template <typename T> bool get_payload(uint32_t begin, uint32_t i, T &out) const {
		return pod_read(payload, begin + (size_t)i * pod_words<T>(), out);
	}
	// Raw uint32 (a vtable-group slot id).
	uint32_t add_word(uint32_t w) { uint32_t off = (uint32_t)payload.size(); payload.push_back(w); return off; }
	bool get_word(uint32_t begin, uint32_t i, uint32_t &out) const {
		size_t off = begin + i;
		if ( off >= payload.size() ) return false;
		out = payload[off];
		return true;
	}
};

// The LOAD side: a READ-ONLY view over the three blocks placed in loaded/mmap'd memory —
// the eventual `--forest-bind` shape. `defs`/`payload` are const uint32 spans bound in
// place (indices/offsets need no fixup); strings resolve through a frozen_intern_table
// bound to the serialized intern blocks. This is what the live handle wraps after a load;
// the accessor surface mirrors DefArena so the same decode logic reads either.
class FrozenDefArena
{
public:
	frozen_intern_table  strings;
	const uint32_t      *defs;         size_t defs_words;
	const uint32_t      *payload;      size_t payload_words;

	FrozenDefArena() : defs(0), defs_words(0), payload(0), payload_words(0) {}

	void bind_defs(const uint32_t *p, size_t words)    { defs = p; defs_words = words; }
	void bind_payload(const uint32_t *p, size_t words) { payload = p; payload_words = words; }

	uint32_t def_count() const { return (uint32_t)(defs_words / DefArena::def_stride()); }

	bool get_def(uint32_t idx, defrec &out) const {
		size_t off = (size_t)idx * DefArena::def_stride();
		if ( !defs || off + DefArena::def_stride() > defs_words ) return false;
		memcpy(&out, defs + off, sizeof(defrec));
		return true;
	}
	template <typename T> bool get_payload(uint32_t begin, uint32_t i, T &out) const {
		size_t off = (size_t)begin + (size_t)i * pod_words<T>();
		if ( !payload || off + pod_words<T>() > payload_words ) return false;
		memcpy(&out, payload + off, sizeof(T));
		return true;
	}
	bool get_word(uint32_t begin, uint32_t i, uint32_t &out) const {
		size_t off = (size_t)begin + i;
		if ( !payload || off >= payload_words ) return false;
		out = payload[off];
		return true;
	}
	const char *c_str(uint32_t id) const { return strings.c_str(id); }
};

} // namespace dis
} // namespace madc

#endif // __CIR_ARENA_H
