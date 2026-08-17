#ifndef __CIR_ARENA_H
#define __CIR_ARENA_H 1

// cir_arena.h — B3 arena-native DataDef storage (step-2 skeleton).
//
// The forest campaign's endgame (docs/plans/2026-07-06-forest-arena-native-scoping.md
// + 2026-07-06-forest-b3-record-layout-DESIGN.md): a DataDef's COMPLETE state lives as a
// flat POD record in one contiguous arena — cross-references are TYPE-IDS (the spine, not
// heap pointers), identifiers are intern-pool offsets, and variable-length collections are
// (begin,count) slices into a side payload. SAVE = dump the three blocks; LOAD = read them
// back with ZERO pointer fixup (everything is already id/offset relative). The live DataDef
// eventually becomes a thin HANDLE over a `defrec`; this header is the storage that handle
// wraps.
//
// KEYED BY TYPE-ID (the spine — "uid is the spine"). The arena stores the PROJECT segment of
// the type-id space (madc_typeid.h): the `defs` array is addressed by PROJECT-ID SLOT —
// record slot k <=> project type_id (MADC_TYPEID_PROJECT_BASE + k), mirroring the live
// id_table<DataDef> the freeze already walks in id order (cir_freeze.cpp). A cross-reference
// (a pointer's pointee, a member/param/return type, a base class, a method's FuncDef) is
// stored as the referent's SERIALIZED type_id, and resolves on load exactly like the live
// madc_type_from_id dispatch:
//   * PINNED primitive (id < MADC_TYPEID_PRIMITIVE_END) — void/int/char/char*/... — is NEVER
//     recorded in the arena; the id itself resolves to the loading process's own primitive
//     global. (This is the correction to the step-1 spike, which over-encoded `int` as a
//     DK_PRIM record.)
//   * PROJECT type (id >= MADC_TYPEID_PROJECT_BASE) — its record lives at slot (id - base).
// (The SYSTEM segment [0x100, PROJECT_BASE) is reserved for the embedded forest, unused here.)
//
// It is built ON the existing substrate (madc::dis::intern_table + pod_record) — NOT a
// parallel arena. The forest's cir_forest_type_record family is the same idea as a
// serialization mirror; B3 promotes this to canonical live storage. The save-side cross-ref
// policy is madc_cir.cpp's forest_serialize_type_id (pinned-as-slot, scalar-alias-normalized,
// else project id) — reuse it, do not reinvent it.
//
// PAYLOAD DISCIPLINE (load-bearing): every variable-length run (members, bases, methods,
// vbase pairs, vtable-group recs, vtable-group SLOT id runs, func params) is packed into the
// ONE `payload` block. A run must be CONTIGUOUS, so an encoder MUST resolve every child
// cross-ref FIRST (the recursive encode of a member/param/base TYPE may itself append payload)
// and only THEN append its own run. Capturing begin=payload.size() before a loop that recurses
// would interleave a nested aggregate's run with this one.
//
// The live-handle migration is underway: a DefArena lives on Program (Program::forest_arena),
// and the unary derived-type funnels — getPointerType / getReferenceType / getConstType —
// write-through a new project pointer / reference / const record into it (via
// Program::forest_arena_record_unary) when Program::forest_arena_enabled is set (SLICE 1c/1d).
// That runtime flag — default off, so bin/madc is unchanged — is the testable realization of the
// design's FEATURE_FOREST_ARENA guard (a #ifdef could not be on-for-test / off-for-ship in the
// shared parser.o). test_cir_arena.cpp exercises the schema round-trip AND the live write-through.

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

#include "madc_typeid.h"
#include "madcdis/intern_table.h"
#include "madcdis/pod_record.h"

namespace madc {
namespace dis {

// Discriminant for a defrec — replaces the vtable for STORAGE purposes (the live handle
// still dispatches virtually; the tag drives (de)serialization + handle construction).
// A PINNED primitive is never recorded (referenced by its pinned id) — DK_PRIM/DK_VOID
// remain reserved for a future need, never emitted by the current model.
enum DefKind : uint32_t {
	DK_NONE = 0,	// unset slot (a project id with no record yet)
	DK_PRIM,	// (reserved — primitives are referenced by pinned id, not recorded)
	DK_VOID,	// (reserved)
	DK_PTR,		// ref0 = pointee type-id
	DK_REF,		// ref0 = referee type-id
	DK_CONST,	// ref0 = unqualified type-id
	DK_ENUM,
	DK_STRUCT,	// members_* slice
	DK_UNION,	// members_* slice, union layout
	DK_CLASS,	// members_* + bases_* + methods_* + vbase_* + vgroup_* (the flattenings)
	DK_FUNC,	// FuncDef: ref0 = return type-id, params_* slice
	DK_VAR,		// Variable: ref0 = type type-id (later increment)
	DK_SIMD,
	DK_FPTR,	// v22: DataDefFPTR — ref0 = the target FuncDef's DK_FUNC record
	DK_MEMBERPTR,
	DK_CARRAY,	// v25: DataDefCArray — ref0 = element type-id; carray_count_lo/hi
			// hold the FOLDED element count (a runtime-sized array —
			// count_expr set — is never recorded; it is function-local
			// state, not header state, and cleanly lacks)
	DK_TYPEDEF,	// ref0 = underlying type-id (a named alias; ns_id gives its namespace)
	DK_NSLINK,	// v25: an INLINE-namespace link (Program::inline_namespace_children):
			// ns_id = parent namespace (0 = global), name_id = the inline child's
			// full name ("std::__cxx11"). The flush re-runs the ONE live
			// derivation (mirror_inline_namespace_into_parent) over restored
			// state, so std::__cxx11-defined names (stod, to_string) resolve
			// as members of std, exactly as a live parse leaves them.
	DK_NSBIND,	// v25: a USING-DECLARATION function import (`namespace std {
			// using ::abort; }`): ns_id = the importing namespace, name_id =
			// the visible name, disp_id = the imported fn's funcdef_map key.
			// The flush rebinds namespace_map[ns][name] to the restored fn's
			// Variable — the exact live registration the using-decl performs.
	DK_DEFBODY,	// v26: one Program::deferred_lazy_bodies entry — a method body the
			// producer never ODR-used (live materializes it from TOKENS via
			// parse_deferred_lazy_body on first use; the frozen AST has no
			// func-def for it). Per-kind field reuse (no layout growth):
			//   name_id      = the map key (== the method Variable's name)
			//   ref0         = the owner class's type-id (0 = none)
			//   disp_id      = origin FILE intern id (0 = none)
			//   body_unit    = line, body_idx = column
			//   flags        = DF_DEFBODY_FULL_DEFINITION when full_definition
			//   params_begin = word offset of FOUR token-run descriptors in the
			//                  arena payload (off/bytes/count/file_id each, in
			//                  DeferredFunctionBody field order: body /
			//                  definition / trailing_ret / ctor_init)
			//   params_count = 4
			// The flush rebuilds the entry (var = the restored method
			// Variable) so the EXISTING materialize-and-lower fixpoint
			// re-runs the one live derivation on first ODR-use.
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
	DF_IS_CONST_METHOD   = 1u << 13,	// FuncDef::is_const_method
	DF_HAS_FOREST_BODY   = 1u << 14,	// INLINE method: body_unit/body_idx locate its Tree-1 def
	DF_IS_MEMBER_TEMPLATE = 1u << 15,	// FuncDef::is_member_template / template_param_names
						// non-empty — a template method instantiates no
						// concrete symbol; load skips it (the v6 rule)
	DF_IS_FREE_FUNC      = 1u << 16,	// RC2: a file-scope FREE function (not a class
						// method) — name_id is its funcdef_map key / call
						// name; load restores funcdef_map + a program-scope
						// Variable so a bound call resolves the real
						// signature instead of the dlsym variadic fallback
	DF_WAS_BODIED        = 1u << 17,	// v20: the free function had a BODY at freeze
						// (an instantiated __mti / __ns_*__oN definition, or
						// a producer root like main). Load restores it ONLY
						// when a forest body location was stamped
						// (DF_HAS_FOREST_BODY, system-header origin) —
						// otherwise it cleanly lacks (a producer root must
						// never restore into a consumer).
	DF_TRET_FROM_POINTER = 1u << 18,	// v21: FuncDef::template_return_deduce_from_pointer
	DF_TRET_REF          = 1u << 19,	// v21: FuncDef::template_return_ref

	DF_FPTR_PTR_SYNTAX   = 1u << 20,	// v22: DataDefFPTR::ptr_syntax (explicit `(*)` form
						// vs a Form-1 function typedef)

	DF_DEFBODY_FULL_DEFINITION = 1u << 22,	// v26: DeferredFunctionBody::full_definition
	DF_TU_ROOT_ORIGIN    = 1u << 21,	// v24: defined in the TU's ROOT file (the program
						// itself, not an #include) — the record stays in
						// the arena for --run-frozen's typeid->name
						// closure, but the FOREST/bind restore fences it
						// out (the forest holds #include state ONLY)
	DF_TYPEDEF_FLAT_ALIAS = 1u << 23,	// v26: a namespaced DK_TYPEDEF alias key the
						// producer's FLAT datatype_map ALSO holds under
						// the same key -> same definition (the explicit-
						// specialization alias_key surface,
						// TokenTEMPLATE::parse) — the restore reproduces
						// the flat datatype_map + struct_map writes too,
						// not just the namespace map
	DF_PURE_VIRTUAL      = 1u << 25,	// FuncDef::pure_virtual (`= 0`): the class is
						// abstract while this slot has no final
						// overrider; the vtable slot fills with
						// __cxa_pure_virtual. Flag-bit addition only —
						// no record layout change, no version bump.
	DF_FUNC_DEF_TOKENS   = 1u << 24,	// v26 piece (a): this WAS_BODIED free fn (no
						// TRANSLATED def -> no DF_HAS_FOREST_BODY) has an
						// ownerless DK_DEFBODY twin carrying its raw body
						// tokens — the declaration restores (the
						// was_bodied&&!has_body drop lifts) and the
						// deferred body materializes on first ODR-use
	DF_TYPEDEF_TAG_ALIAS = 1u << 25,	// a FLAT DK_TYPEDEF alias the producer's
						// struct_map ALSO holds under the same key ->
						// same definition (the struct/class-KEYWORD
						// typedef forms: `typedef struct [tag] {...} X;`
						// / `typedef struct tag X;` register the alias
						// as a tag so `struct X` resolves; the plain
						// TokenTYPEDEF path never does) — the restore
						// reproduces the struct_map write
	DF_OPAQUE_TAG        = 1u << 27,	// a CONCRETE opaque forward tag (an empty-pack
						// recursion tail like _Tuple_impl<1>, minted
						// OUTSIDE a dependent parse): records in the v21
						// empty-incomplete shape; the restore re-stamps
						// is_dependent_placeholder + opaque_concrete_tag
						// so the consumer's dependence classification
						// matches a live parse (LOADED == parsed)
	DF_CLASS_FN_LOCAL    = 1u << 28,	// aggregate: a FUNCTION-LOCAL class (hoisted
						// local class of a fn/method body — Guard,
						// _Save_errno). C++ scoping makes it unnameable
						// outside its function, so a bound consumer can
						// never DEMAND it by name: it must not seed the
						// admitted-set chase (startup R1); it stays
						// reachable through reference pulls / its owner
						// body's use.
	DF_BODY_IN_INSTANTIATION = 1u << 26,	// v27: the captured DEFBODY tokens were parsed
						// inside a fn-template INSTANTIATION
						// (fn_template_instantiation_depth > 0 — an
						// instantiated __oN definition, __stoa__o2); the
						// ownerless DEFBODY re-run reproduces that
						// context so the local-class reuse allowance
						// (TokenCLASS::parse, `struct _Save_errno`)
						// applies exactly as live
	DF_NOEXCEPT_TRUE     = 1u << 29,	// v35: FuncDef::noexcept_spec == NxTrue
	DF_NOEXCEPT_UNKNOWN  = 1u << 30,	// v35: FuncDef::noexcept_spec == NxUnknown
						// (neither bit -> NxNone); consumed by the
						// __is_nothrow_constructible trait so a thawed
						// class folds it exactly as live (LOADED==PARSED)
	DF_ENUM_CLASS_NESTED = 1u << 31,	// DK_ENUM-scoped: the tag is a MEMBER of a class
						// ([basic.scope.class]/1), so a live parse keeps
						// it OUT of datatype_map / the namespace and
						// registers it ONLY as the owner's type alias
						// (parser.cpp TokenENUM::parse — money_base::part
						// leaked `part` at file scope before that fix).
						// The record exists so the tag's project id
						// resolves as a member / param / fn-ptr signature
						// type; the restore must NOT flat-register it
						// (LOADED == parsed) — the owner's type_aliases
						// restore is its whole registration
	DF_NSBIND_OVERLOAD_MEMBER = 1u << 0,	// DK_NSBIND-scoped: the imported fn is a MEMBER
						// of ns::name's overload set ([namespace.udecl]
						// join — the using-arm's second registration);
						// the flush joins it back so a bound consumer
						// ranks the SAME set the freezing parse ranked
						// (LOADED==PARSED; a smaller thaw-side set split
						// instantiation identities — stl_vector.h:428)
};

// Per-method flag bits (methodrec.flags) — the class-membership classification the live
// path derives structurally (ctor set / "~" dtor key / static). Return + params + the
// FuncDef-intrinsic flags live on the referenced DK_FUNC defrec.
enum MethodFlags : uint32_t {
	MF_CTOR   = 1u << 0,	// joins DataDefCLASS::ctors on load
	MF_DTOR   = 1u << 1,	// the class_own_dtor ("~") method_map key
	MF_STATIC = 1u << 2,	// no hidden __this
};

// Per-base flag bits (baserec.flags).
enum BaseFlags : uint32_t {
	BSF_VIRTUAL = 1u << 0,
	BSF_PRIMARY = 1u << 1,
	// BaseSpec::access rides bits 8+ (the vf* flags fit in a byte).
	BSF_ACCESS_SHIFT = 8,
};

// One fixed-stride POD record per project DataDef. uint32 words only (pod_record requires
// whole words + trivial copyability). Cross-refs are TYPE-IDS (pinned or project); strings
// are intern ids; collections are (begin_word, count) slices into the arena payload. Unused
// fields are 0.
struct defrec {
	uint32_t kind;		// DefKind
	uint32_t name_id;	// intern id of DataDef::name
	uint32_t canon_id;	// intern id of canonical_cpp_spelling (0 = none)
	uint32_t size;		// DataDef::size
	uint32_t datatype;	// the originating DataType enum value (rawtype seed)
	uint32_t flags;		// DefFlags
	uint32_t ns_id;		// defining-namespace intern id (0 = global)
	uint32_t ref0;		// PTR/REF/CONST: operand type-id; FUNC: return type-id; VAR: type type-id; ENUM: fixed-underlying type-id; else 0
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
	uint32_t vbase_begin;	// vbaserec run (sorted by class_id)
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
	// FUNC (DK_FUNC) method fidelity — the FuncDef-intrinsic state materialize reads:
	uint32_t emit_symbol_id;	// FuncDef::emit_symbol intern id (0 = none; LIBRARY link symbol)
	uint32_t disp_id;		// FuncDef::method_display_name intern id (0 = none)
	uint32_t body_unit;		// INLINE body location (valid iff DF_HAS_FOREST_BODY)
	uint32_t body_idx;
	// struct/class anonymous sub-aggregate groups (flattened anon union/struct):
	uint32_t anon_begin;	// anonrec run
	uint32_t anon_count;
	// class-scope name maps (v20, widening slice 2): the lookups class-member
	// TYPE resolution + template machinery read — type_aliases (typedef/using
	// aliases: `typename _Alloc::value_type`), static_member_types, and
	// static_member_const_values (`X<T>::value`, the integral_constant fold).
	uint32_t alias_begin;	// aliasrec run (DataDefCLASS::type_aliases)
	uint32_t alias_count;
	uint32_t statty_begin;	// aliasrec run (DataDefCLASS::static_member_types)
	uint32_t statty_count;
	uint32_t constval_begin; // constvalrec run (DataDefCLASS::static_member_const_values)
	uint32_t constval_count;
	// FUNC (DK_FUNC) free-function fidelity (v21, widening slice 3): the
	// remaining FuncDef-intrinsic state a skipped-ns-fn-template PLACEHOLDER
	// (__ns_std__Destroy) and an instantiated __oN definition carry —
	// inline_builtin_kind ("addressof"/"destroy"/"forward") and the
	// identity-return deduce pattern (template_return_param_name +
	// arg index; the two bools ride DF_TRET_* flags). On a DF_IS_FREE_FUNC
	// record, disp_id holds FuncDef::function_display_name (a free fn's
	// method_display_name is always empty) and ns_id holds namespace_name.
	uint32_t builtin_kind_id;	// intern id of inline_builtin_kind (0 = none)
	uint32_t tret_name_id;		// intern id of template_return_param_name (0 = none)
	uint32_t tret_arg_index;	// template_return_deduce_arg_index
	// CARRAY (DK_CARRAY, v25): DataDefCArray::count (carray_dim_t is 64-bit),
	// split into two words like constvalrec's value.
	uint32_t carray_count_lo;
	uint32_t carray_count_hi;
};

// A class-scope name -> type binding (type_aliases / static_member_types).
struct aliasrec {
	uint32_t name_id;	// the alias / member name
	uint32_t type_id;	// its type (pinned or project)
};

// An integral static-const data member's compile-time value
// (DataDefCLASS::static_member_const_values — the std::integral_constant
// pattern; `X::value` reads the real value, not a placeholder).
struct constvalrec {
	uint32_t name_id;
	uint32_t val_lo;	// int64_t, split into two words
	uint32_t val_hi;
};

// One member of a STRUCT/CLASS — the complete per-member state materialize reads
// (offset/count/access/origin/bitfield loaded VERBATIM). uint32 words only.
struct memberrec {
	uint32_t name_id;	// memberpair_t.first
	uint32_t type_id;	// memberpair_t.second, as a type-id (pinned or project)
	uint32_t typedef_id;	// memberpair_t.typedef_name (0 = none)
	uint32_t offset;	// member_offsets[i]
	uint32_t count;		// member_counts[i]
	uint32_t flags;		// bit0 = array_flag (grows)
	uint32_t access;	// member_access[i] (vf* access bits)
	int32_t  origin;	// member_origin[i] — owning base index, or -1 for an own member
	// bitfield (member_bitfields[i]); bf_flags bit0=is_bitfield, bit1=is_unsigned, bit2=reverse_storage
	uint32_t bf_flags;
	uint32_t bf_bit_offset;
	uint32_t bf_bit_width;
	uint32_t bf_storage_offset;
	uint32_t bf_storage_size;
	// v32: member_vbase[i] — the VIRTUAL base this member belongs to, as a
	// type-id (0 = not a virtual-base member). Member access through a vbase
	// VIEW reads this to pick the dynamic (vtable-slot) adjust; without it a
	// restored header class silently degraded to the static offset.
	uint32_t vbase_id;
};

// A direct base (DataDefCLASS::bases -> BaseSpec).
struct baserec {
	uint32_t base_id;	// BaseSpec.base, as a type-id
	uint32_t offset;	// BaseSpec.offset
	uint32_t flags;		// BaseFlags (is_virtual | is_primary | access<<8)
};

// A method (DataDefCLASS::methods -> Variable* wrapping a FuncDef*). Return + params +
// the FuncDef-intrinsic flags (const/varargs/void-params/decl-only/body) live on the
// referenced DK_FUNC defrec (func_id); this rec carries the class-membership state.
struct methodrec {
	uint32_t name_id;	// Variable::name (the mangled call symbol)
	uint32_t func_id;	// the method's FuncDef, as a type-id (DK_FUNC record)
	uint32_t flags;		// MethodFlags (MF_CTOR | MF_DTOR | MF_STATIC)
	uint32_t disp_key_id;	// method_map KEY intern id (== method_display_name for a plain
				// method/operator; the "~" tag for the dtor; 0 for a concrete ctor)
};

// A flattened anonymous sub-aggregate group (DataDefSTRUCT::AnonymousAggregateInfo): the
// nameless union/struct's members are already in the parent's member run; this relinks the
// grouping + names the sub-aggregate by type-id so emission re-nests the overlap.
struct anonrec {
	uint32_t first_member;	// index into the parent's member run
	uint32_t member_count;
	uint32_t offset;
	uint32_t sub_type_id;	// the nameless sub-aggregate, as a type-id
};

// A flattened virtual-base offset (the pointer-KEYED map, sorted by class_id).
struct vbaserec {
	uint32_t class_id;	// the virtual base, as a type-id
	uint32_t offset;	// its subobject offset
};

// A vtable group (DataDefCLASS::VtableGroup): the nested `slots` vector is a SEPARATE
// (slots_begin, slots_count) run of raw intern ids in the payload.
struct vgrouprec {
	uint32_t owner_id;	// VtableGroup.owner, as a type-id
	uint32_t this_offset;	// VtableGroup.this_offset
	uint32_t slots_begin;	// WORD offset of the slot-id run (uint32 name_ids)
	uint32_t slots_count;	// number of slot ids
	uint32_t addr_point;	// VtableGroup.addr_point
};

// A function parameter (FuncDef::parameters[i]).
struct paramrec {
	uint32_t type_id;	// the parameter type, as a type-id
	uint32_t flags;		// bit0 = const_param (grows)
	uint32_t cpp_spelling_id;	// param_cpp_spellings[i] (0 = render from type)
	// v23: the parameter's DEFAULT-ARGUMENT expression, as its RAW SOURCE
	// TOKEN run (FuncDef::param_default_tokens[i], .madh record form) in the
	// arena's tokbytes block. param_defaults[i] is a PARSED TREE the codec
	// cannot carry; the load re-runs parseExpression over these tokens at
	// the pending-funcs flush — the one live derivation. 0 bytes = no
	// default. def_file_id interns the run's origin file (arena strings).
	uint32_t def_tok_off;	// BYTE offset into DefArena::tokbytes
	uint32_t def_tok_bytes;	// byte length (0 = no default captured)
	uint32_t def_tok_count;	// token count in the run
	uint32_t def_file_id;	// origin-file intern id (0 = none)
};

// Type-id segment predicates (the spine). A cross-ref stored in a defrec is one of:
//   INVALID (0)  — a null referent
//   PINNED       — a primitive, [1, MADC_TYPEID_PRIMITIVE_END): NOT recorded; resolve via
//                  madc_type_from_id to the process global
//   SYSTEM       — [MADC_TYPEID_PRIMITIVE_END, MADC_TYPEID_PROJECT_BASE): embedded forest (unused here)
//   PROJECT      — [MADC_TYPEID_PROJECT_BASE, ...): a record at slot (id - PROJECT_BASE)
inline bool arena_id_is_pinned(uint32_t type_id)
{
	return type_id != (uint32_t)MADC_TYPEID_INVALID && type_id < (uint32_t)MADC_TYPEID_PRIMITIVE_END;
}
inline bool arena_id_is_project(uint32_t type_id) { return type_id >= MADC_TYPEID_PROJECT_BASE; }
inline uint32_t arena_slot_of(uint32_t project_id) { return project_id - MADC_TYPEID_PROJECT_BASE; }
inline uint32_t arena_id_of(uint32_t slot) { return MADC_TYPEID_PROJECT_BASE + slot; }

// The arena: three self-contained, id/offset-addressed blocks. A byte dump is the three
// blocks concatenated; a load reads them back with no fixup (this is why round-tripping is a
// plain copy of the vectors — no live pointer ever enters the arena). `defs` is addressed by
// PROJECT-ID SLOT (slot k <=> project id PROJECT_BASE + k); primitives are never recorded.
class DefArena {
public:
	intern_table          strings;	// every identifier / spelling
	std::vector<uint32_t> defs;	// defrec[] addressed by project-id slot (id - PROJECT_BASE)
	std::vector<uint32_t> payload;	// memberrec/baserec/methodrec/vbaserec/vgrouprec/paramrec + slot-id runs
	std::vector<uint8_t>  tokbytes;	// v23: raw token runs (.madh record form) —
					// param-default expressions; runs referenced
					// by BYTE offset from paramrec.def_tok_off

	static uint32_t def_stride() { return (uint32_t)pod_words<defrec>(); }
	uint32_t def_slots() const { return (uint32_t)(defs.size() / def_stride()); }

	// Write a PROJECT type's record at its id slot (resize to fit). type_id MUST be a
	// project id — primitives are never recorded (referenced by their pinned id). The
	// two-phase build re-sets the same slot once children/runs are known.
	void set_def_at(uint32_t project_id, const defrec &r)
	{
		uint32_t slot = arena_slot_of(project_id);
		size_t need = ((size_t)slot + 1) * def_stride();
		if ( defs.size() < need ) defs.resize(need, 0u);
		std::memcpy(&defs[(size_t)slot * def_stride()], &r, sizeof(defrec));
	}
	// Read the record for a project id. False for a non-project id or an out-of-range /
	// never-written slot.
	bool get_def_at(uint32_t project_id, defrec &out) const
	{
		if ( !arena_id_is_project(project_id) ) return false;
		return pod_read(defs, (size_t)arena_slot_of(project_id) * def_stride(), out);
	}
	// Is a record present (kind != DK_NONE) at this project id?
	bool has_def(uint32_t project_id) const
	{
		defrec r;
		return get_def_at(project_id, r) && r.kind != DK_NONE;
	}

	// Generic run helpers: append a POD record to payload (returns its word offset), and
	// read the i-th record of a run beginning at `begin`.
	template <typename T> uint32_t add_payload(const T &rec) { return pod_append(payload, rec); }
	template <typename T> bool get_payload(uint32_t begin, uint32_t i, T &out) const {
		return pod_read(payload, begin + (size_t)i * pod_words<T>(), out);
	}
	// Append a raw token-byte run (returns its BYTE offset in tokbytes).
	uint32_t add_tokbytes(const std::vector<uint8_t> &bytes)
	{
		uint32_t off = (uint32_t)tokbytes.size();
		tokbytes.insert(tokbytes.end(), bytes.begin(), bytes.end());
		return off;
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
// place (ids/offsets need no fixup); strings resolve through a frozen_intern_table bound to
// the serialized intern blocks. This is what the live handle wraps after a load; the accessor
// surface mirrors DefArena so the same decode logic reads either. `defs` is addressed by
// project-id slot, exactly as DefArena.
class FrozenDefArena
{
public:
	frozen_intern_table  strings;
	const uint32_t      *defs;         size_t defs_words;
	const uint32_t      *payload;      size_t payload_words;
	const uint8_t       *tokbytes;     size_t tokbytes_len;	// v23 (may be absent: 0)

	FrozenDefArena() : defs(0), defs_words(0), payload(0), payload_words(0),
			   tokbytes(0), tokbytes_len(0) {}

	void bind_defs(const uint32_t *p, size_t words)    { defs = p; defs_words = words; }
	void bind_payload(const uint32_t *p, size_t words) { payload = p; payload_words = words; }
	void bind_tokbytes(const uint8_t *p, size_t len)   { tokbytes = p; tokbytes_len = len; }
	// A token run's bytes (paramrec.def_tok_off/def_tok_bytes); NULL if out of range.
	const uint8_t *tok_run(uint32_t off, uint32_t len) const
	{
		if ( !tokbytes || !len || (size_t)off + len > tokbytes_len ) return NULL;
		return tokbytes + off;
	}

	uint32_t def_slots() const { return (uint32_t)(defs_words / DefArena::def_stride()); }

	bool get_def_at(uint32_t project_id, defrec &out) const
	{
		if ( !arena_id_is_project(project_id) ) return false;
		size_t off = (size_t)arena_slot_of(project_id) * DefArena::def_stride();
		if ( !defs || off + DefArena::def_stride() > defs_words ) return false;
		memcpy(&out, defs + off, sizeof(defrec));
		return true;
	}
	bool has_def(uint32_t project_id) const
	{
		defrec r;
		return get_def_at(project_id, r) && r.kind != DK_NONE;
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
