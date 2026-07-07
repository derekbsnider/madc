/* cir_freeze.h — freeze/thaw a cir_node subtree through the madc::dis
 * pool-snapshot container (forest Phase 2 / data-substrate Track B2).
 *
 * FREEZE flattens a built cir_node sub-DAG into position-independent flat
 * records: the B1 extension block (typeids / string handles / cir_refs) is
 * already serializable and copies as-is; the c2mir-visible base maps here —
 * scalar payloads inline, string payloads (u.s) to string-pool handles, and
 * the op-link child lists to a CSR-style child-index pool. Sharing (N_SHARE
 * spec reuse) and genuine cycles (the __max_size_type shape) are handled by
 * first-touch index assignment, exactly the discipline the dump/error
 * walkers already use. The walk is iterative — real trees exceed 800 deep.
 *
 * THAW registers a CirFrozenSegment in the B1 segment registry: the frozen
 * records join the SAME (seg, idx) id space as live arenas, and
 * madc_cir_node_for() remains the one resolve chokepoint. node_at()
 * materializes on touch — two-phase (shells first, then child appends, so
 * shares and cycles terminate) — into real pointer-linked cir_nodes at the
 * c2mir edge (forest SETTLED #7: cold = records, materialized = pointers).
 *
 * THE FOREST (B3, multi-segment): cir_freeze_forest partitions the sub-DAG
 * into PER-UNIT segments (a unit = the source file the node's origin token
 * came from; for C++20 modules the same directory key carries the module
 * name — a unit name is an interned spelling, not intrinsically a path).
 * A child reference that crosses units is a CONNECTOR: the high bit of the
 * child entry set, the low bits indexing the owning unit's connector pool,
 * whose entries name (target_unit, target_record). Resolving a connector
 * whose unit is not yet loaded triggers decompress+register of that unit —
 * groves load on demand; nothing but the directory and the string pool is
 * read up front (forest plan SETTLED #4/#6: a connector is a REFERENCE,
 * never a node kind, so c2mir stays blind).
 *
 * CROSS-PROCESS CLOSURE (B3): the container carries its own string pool
 * (the A1 frozen_intern_table blocks), a per-record source-position
 * side-car, the typeid->name closure, and the required-library list, so a
 * FRESH process can thaw, compile, and run the tree without the process
 * that froze it: string payloads read from the container pool; extension
 * string ids re-intern into the live pool at materialize (in-process this
 * dedups back to the identical id); positions come from the side-car when
 * the freezing process's token arena is absent. datadef_id stays raw data:
 * primitive-segment ids resolve everywhere (pinned slots); project-segment
 * ids resolve NULL in a foreign process (the compile path never reads
 * them) and are nameable via the typeid->name closure. Rebuilding DataDefs
 * from thawed decl trees is the parser-resume slice (B4+), not B3.
 *
 * CONTEXT-HASH PIN: madc_cir_context_hash() folds the madc version, the
 * record/position layouts, the c2mir node-code enum tail, and the typeid
 * primitive tail. Writers stamp it into the container header; readers
 * REJECT a mismatch (never silently thaw a layout-mismatched forest).
 *
 * B4 hooks reserved here: each directory unit carries an anchor record
 * (CIR_FOREST_ANCHOR_NONE in B3) — the grove entry a parse-time #include
 * or C++20 `import` will bind to instead of re-parsing.
 */

#ifndef __CIR_FREEZE_H
#define __CIR_FREEZE_H 1

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "cir_node.h"
#include "madcdis/intern_table.h"
#include "madcdis/snapshot.h"
#include "madcdis/pod_record.h"	// pod_append / pod_read / pod_words — the fixed-stride record codec
#include "cir_arena.h"		// B3 DefArena / FrozenDefArena — the flip's arena-native store

// Consumer-defined segment kinds for the content-blind snapshot container
// (a container may hold several logical payloads; the kind is the contract).
enum : uint32_t
{
	SNAP_KIND_CIR_RECORDS    = madc::dis::SNAP_KIND_CONSUMER + 0,	// cir_frozen_record[]
	SNAP_KIND_CIR_CHILDREN   = madc::dis::SNAP_KIND_CONSUMER + 1,	// uint32[] record indices
	SNAP_KIND_CIR_FOREST_DIR = madc::dis::SNAP_KIND_CONSUMER + 2,	// cir_forest_dir_header + units + libs
	SNAP_KIND_CIR_CONNECTORS = madc::dis::SNAP_KIND_CONSUMER + 3,	// uint64[] (unit<<32 | record)
	SNAP_KIND_CIR_POSITIONS  = madc::dis::SNAP_KIND_CONSUMER + 4,	// cir_frozen_pos[] parallel to records
	SNAP_KIND_CIR_TYPE_RECORDS = madc::dis::SNAP_KIND_CONSUMER + 5,	// cir_forest_type_record[] (complete DataDef content)
	// --- grove payload v2 (B4a; design doc 2026-07-04 §2) ---
	SNAP_KIND_CIR_UNIT_TOKENS   = madc::dis::SNAP_KIND_CONSUMER + 6,  // post-PP token slice (.madh record form)
	SNAP_KIND_CIR_DECL_INDEX    = madc::dis::SNAP_KIND_CONSUMER + 7,  // cir_forest_decl_entry[]
	SNAP_KIND_CIR_PP_EXPORTS    = madc::dis::SNAP_KIND_CONSUMER + 8,  // uint32 event stream (cir_forest_pp_event + params)
	SNAP_KIND_CIR_UNIT_EDGES    = madc::dis::SNAP_KIND_CONSUMER + 9,  // uint32[] directory unit indices, include order
	SNAP_KIND_CIR_BRANCH_MACROS = madc::dis::SNAP_KIND_CONSUMER + 10, // container-global: uint32 name ids, sorted
	SNAP_KIND_CIR_CANON_ORDER   = madc::dis::SNAP_KIND_CONSUMER + 11, // container-global: uint32 unit indices, canonical order
	// --- Phase 6: complete type-table serialization (2026-06-12 type-table
	// design §6.4 "forest type-refs serialize as ids + table segments") ---
	SNAP_KIND_CIR_TYPE_PAYLOAD  = madc::dis::SNAP_KIND_CONSUMER + 12, // container-global: uint32 member/base payload stream
	SNAP_KIND_CIR_GLOBALS       = madc::dis::SNAP_KIND_CONSUMER + 13, // container-global: cir_forest_global_record[] (file-scope global var defs)
	// --- B3 flip: the DefArena dump (madc::dis::defrec[] + payload u32[]); its intern
	// strings reuse SNAP_KIND_INTERN_* under distinct arena seg-ids. SAVE side first. ---
	SNAP_KIND_CIR_ARENA_DEFS    = madc::dis::SNAP_KIND_CONSUMER + 14, // container-global: madc::dis::defrec[] (arena, id-addressed)
	SNAP_KIND_CIR_ARENA_PAYLOAD = madc::dis::SNAP_KIND_CONSUMER + 15  // container-global: uint32 arena payload stream
};

// One frozen node record (fixed-size POD; x86-64 little-endian first, like
// the container). Child linkage lives in the separate child-index pool:
// records[i] owns children[child_base .. child_base+nchildren). Entries are
// in-segment record indices for B2; the high bit is RESERVED for the B3
// cross-segment connector form.
struct cir_frozen_record
{
	uint32_t code;			// c2mir_node_code_t
	uint32_t nchildren;
	uint64_t child_base;		// first entry in the child-index pool

	uint8_t  payload[16];		// scalar leaf image of node.u (ld = 16 bytes)
	uint32_t str_id;		// string leaf payload: strpool handle (0 = none)
	uint32_t str_len;		// exact stored byte length (as u.s.len)

	// --- B1 extension block: already position-independent, copied as-is ---
	uint32_t origin_id;
	uint32_t datadef_id;
	uint32_t typedef_name_id;
	uint32_t error_msg_id;
	uint32_t tsubst_pack_index;
	uint32_t tsubst_pack_value_id;
	cir_ref  tree1_origin;
	uint8_t  src_lang;
	uint8_t  flags;			// bit0 = synth_from_origin, bit1 = tsubst_pack_expand
	uint8_t  _pad[6];
};

enum : uint8_t
{
	CIR_FROZEN_SYNTH_FROM_ORIGIN = 1u << 0,
	CIR_FROZEN_PACK_EXPAND       = 1u << 1
};

// Child-entry high bit: the entry is a CONNECTOR — the low 31 bits index the
// owning unit's connector pool, whose uint64 entries are (unit << 32 | record).
enum : uint32_t { CIR_FROZEN_CHILD_CONNECTOR_BIT = 0x80000000u };

// A frozen subtree in memory: record 0 is the root.
struct cir_frozen_blob
{
	std::vector<cir_frozen_record> records;
	std::vector<uint32_t>          children;
};

// Flatten the sub-DAG rooted at `root` (iterative; share/cycle-safe).
// False on a null root.
bool cir_freeze_subtree(cir_node *root, cir_frozen_blob &out);

// Stage the blob into a snapshot container as two segments
// (seg_id_base + 0 = records, + 1 = child pool).
bool cir_freeze_write(const cir_frozen_blob &blob,
		      madc::dis::snapshot_writer &w, uint32_t seg_id_base,
		      PchCompression codec = PchCompression::Zlib);

// Read the two segments back out of an opened container. False if either
// segment is missing, fails to decompress, or has a malformed size.
bool cir_freeze_read(const madc::dis::snapshot_reader &r, uint32_t seg_id_base,
		     cir_frozen_blob &out);

// ---------------------------------------------------------------------------
// The forest (B3): per-unit segments + connectors + cross-process closure
// ---------------------------------------------------------------------------

// Format 2 = grove payload v2 (B4a): four per-unit segment slots grew to
// eight (tokens / decl index / PP exports / edges) plus the two container-
// global segments (branch macros, canonical order). The version feeds the
// context hash, so v1 readers reject v2 containers and vice versa.
enum : uint32_t { CIR_FOREST_FORMAT_VERSION = 17 };	// v17: additionally dump the B3 DefArena (defrec[] + payload u32[] + its intern strings) as arena segments 10-14 alongside the v6 type records — the flip's SAVE side; not yet read on load (bind still uses the v6 type records), so this is additive, but the version is bumped because the container CONTENT grew. v16: serialize a bound header's file-scope CLASS-typed global INITIALIZER FORM (cir_forest_global_record.gflags gains CIR_GLOBALF_CLASS_VALUE_INIT / CIR_GLOBALF_CLASS_COPY_TEMP) so load reconstructs a TokenDecl whose emission is byte-identical to a live parse. v13 stored no form -> flush set decl=NULL -> collect_global_ctors' built-in path default-constructed DIRECTLY on the global; a live parse instead runs the class-instance init path: `T x = T()` builds a stack temp via the default ctor then copies it in (COPY_TEMP), while `T x{}` is a trivially-copyable self-copy via try_implicit_copy_construct (VALUE_INIT — needs NO ctor, so in_place, whose ctor was never serialized, binds anyway instead of being dropped by the ctors.empty() guard). v16 ALSO serializes DataDefCLASS::nvsize (the non-virtual size, left behind before v16 — a restored class defaulted nvsize=0): a functional-construction temporary's alloca AND the struct-copy in try_implicit_copy_construct are sized by nvsize, so an empty tag class restored with nvsize=0 emitted its `T x{}` / `T x = T()` global init with NO stack temp (alloca 0) and NO copy loop, unlike live. Together these close the LAST whole-<string>-TU MADC_DUMP_MIR gap (the in_place data item + the piecewise/allocator init-SHAPE divergence, which the top-level item-set diff hid inside __madc_global_init's body). The cir_forest_type_record grew a uint32_t (nvsize) vs v15 (a stale v15 container lacks the form flags -> would re-introduce the direct-construct shape + drop in_place). v15: serialize a class's OWN destructor even when it has neither an external emit_symbol NOR an inline body the producer emitted (a bodyless/unreferenced inline dtor) — declaration-only (CIR_METHF_DTOR, no body). A live parse registers such a dtor (class_own_dtor non-NULL) so Pass 1.6 synthesizes NO Cls___dtor; dropping it from the freeze made the restored class look dtor-less, so a --forest-bind consumer SYNTHESIZED a spurious trivial Cls___dtor that a live compile never emits (the whole-<string>-TU "only in BIND" overshoot: _Save_errno/__new_allocator_*/allocator_*/wide-char basic_string dtors). No layout change vs v14 — bumped because the freeze CONTENT changed (a stale v14 container lacks these dtor records → would re-introduce the overshoot). v14: serialize a bound header's file-scope SCALAR-const global VARIABLE definitions (cir_forest_global_record gains gflags + init_value): a scalar global's init is a compile-time constant (no ctor), so its integer value is stored and load rebuilds a `T name = value;` dkGlobalVar decl — a --forest-bind consumer of <string> now emits its scalar tag globals (hardware_constructive/destructive_interference_size = 64) as data items, byte-identically to live (in_place's {} value-init remains a follow-on: it uses try_implicit_copy_construct, not a ctor); v13: serialize a bound header's file-scope CLASS-typed global VARIABLE definitions (cir_forest_global_record) so load rebuilds them into tkProgram->variables + dkGlobalVar TopDecls and the existing passes emit them + synthesize __madc_global_init (the default ctors come from the v12 ctor set) — a --forest-bind consumer of <string> now emits its inline tag globals (piecewise_construct/allocator_arg) + global-init, as a live parse does; v12: serialize a class's ctors/dtor/operators (CIR_METHF_CTOR/DTOR) instead of skipping them, so load rebuilds cdd->ctors (default-construction resolves) + the "~" dtor method_map key (scope-exit cleanup resolves the D1 symbol) + operator= overloads — a std::string consumer now BINDS and RUNS correctly (constructs, assigns, sizes, destroys; output == live == g++). Whole-<string>-TU MIR byte-identity is NOT yet reached (two separate whole-TU-emission gaps: header global-var defs + __madc_global_init are not serialized; the bind synth-dtor set exceeds live's reachable set) — each its own follow-on slice; v11: serialize a non-polymorphic aggregate's COMPLETE state — anonymous_aggregates (re-nested nameless sub-aggregates, so a struct with an anon union/struct member binds with the right layout) + the layout scalars pack/tag_explicit_align/is_anonymous/reverse_scalar_storage/has_anon_aggregate — instead of a hand-picked field subset; v10: a type record carries its defining namespace (namespace_id) so load restores a namespaced type into namespace_map + namespace_datatype_map (a bound `N::P` / `std::X` resolves), not just the flat maps; v9: derived-type records (CIR_TYPEK_POINTER/REFERENCE/CONST, ref0 = operand typeid) so a pointer/reference/const member (or method param/return, or typedef underlying) serializes as a table entry + swizzles back on load, instead of bailing the whole aggregate; v8: an INLINE method carries its body location (body_unit/body_idx + CIR_METHF_HAS_BODY) so load reconnects the method to its Tree-1 func-def subtree (copied into the consumer's Tree-2 on use, like a template instantiation); a LIBRARY method has no func-def in the AST -> declaration-only + emit_symbol (unchanged); v7: class method declarations (non-virtual) ride the type record (method_begin/count + cir_forest_type_method); v6: complete type-table serialization (typeid->full DataDef, swizzle on load) replaces the typeid->name closure + the decl_record/struct_member parallel streams
enum : uint32_t { CIR_FOREST_ANCHOR_NONE = 0xffffffffu };  // B4 grove-entry hook

// Fixed container segment-id layout for a forest (the directory is the map;
// these are its well-known ids).  Unit i's payloads live at
// CIR_FOREST_SEG_UNIT_BASE + i * CIR_FOREST_SEGS_PER_UNIT + slot.
enum : uint32_t
{
	CIR_FOREST_SEG_DIR           = 1,
	CIR_FOREST_SEG_STR_BYTES     = 2,	// SNAP_KIND_INTERN_BYTES
	CIR_FOREST_SEG_STR_ENTRIES   = 3,	// SNAP_KIND_INTERN_ENTRIES
	CIR_FOREST_SEG_STR_BUCKETS   = 4,	// SNAP_KIND_INTERN_BUCKETS
	CIR_FOREST_SEG_TYPE_RECORDS  = 5,	// cir_forest_type_record[] (complete DataDef content)
	CIR_FOREST_SEG_BRANCH_MACROS = 6,	// v2 (absent = zero-length)
	CIR_FOREST_SEG_CANON_ORDER   = 7,	// v2 (absent = zero-length)
	CIR_FOREST_SEG_TYPE_PAYLOAD  = 8,	// v6 (absent = zero-length): member/base u32 payload stream
	CIR_FOREST_SEG_GLOBALS       = 9,	// v13 (absent = zero-length): file-scope global var defs
	// B3 flip: DefArena dump (10-14). Absent = zero-length. SAVE side (not yet read on load).
	CIR_FOREST_SEG_ARENA_DEFS    = 10,	// madc::dis::defrec[] (id-addressed by project slot)
	CIR_FOREST_SEG_ARENA_PAYLOAD = 11,	// arena payload u32 stream
	CIR_FOREST_SEG_ARENA_STR_BYTES   = 12,	// arena intern bytes   (SNAP_KIND_INTERN_BYTES)
	CIR_FOREST_SEG_ARENA_STR_ENTRIES = 13,	// arena intern entries (SNAP_KIND_INTERN_ENTRIES)
	CIR_FOREST_SEG_ARENA_STR_BUCKETS = 14,	// arena intern buckets (SNAP_KIND_INTERN_BUCKETS)
	CIR_FOREST_SEG_UNIT_BASE     = 16,
	CIR_FOREST_SEGS_PER_UNIT     = 8	// +0 records, +1 children, +2 connectors,
						// +3 positions, +4 tokens, +5 decl index,
						// +6 pp exports, +7 edges (v2 slots may be
						// zero-length: module-only freeze)
};

struct cir_forest_dir_header	// directory payload: header, then units, then lib name ids
{
	uint32_t version;	// CIR_FOREST_FORMAT_VERSION
	uint32_t unit_count;
	uint32_t root_unit;	// the frozen tree's root record
	uint32_t root_idx;
	uint32_t lib_count;	// required dlopen()'d libraries (link-environment closure)
	uint32_t _pad;
};

struct cir_forest_dir_unit
{
	uint32_t unit_name_id;	// pool handle: source path (or C++20 module name — a
				// unit key is an interned spelling, not intrinsically a path)
	uint32_t record_count;
	uint32_t connector_count;
	uint32_t anchor_idx;	// v2: the unit's decl-index ENTRY COUNT when a grove
				// payload exists (the keyframe a parse-time #include /
				// import binds to); CIR_FOREST_ANCHOR_NONE = no grove
				// payload (module-only unit)
};

// --- grove payload v2 PODs (B4a) -------------------------------------------

// One decl-index entry: exported name -> token-slice range in the unit's
// token segment. A name with N registrations has N entries (bind
// materializes the full set — overloads, fwd decl + definition).
struct cir_forest_decl_entry
{
	uint32_t name_id;	// pool handle (exported, namespace-qualified form)
	uint32_t kind;		// Program::PackDeclKind wire value
	uint32_t slice_begin;	// unit-local token indices [begin, end)
	uint32_t slice_end;
	uint32_t aux;		// PACK_DECL_* flags (spans-units / fuzzy-bounds)
};

// One PP-export event head in the unit's uint32 event stream; nparams
// param-name ids follow immediately. tag_flags low byte = the
// Program::PackMacroEvent tag (define / define-fn / undef tombstone);
// bit 8 = variadic.
struct cir_forest_pp_event
{
	uint32_t name_id;
	uint32_t tag_flags;
	uint32_t body_id;	// pool handle: body text (object value / fn body); 0 = none
	uint32_t variadic_param_id;	// pool handle; 0 = unnamed / not variadic
	uint32_t nparams;
};

enum : uint32_t { CIR_FOREST_PP_VARIADIC = 1u << 8 };

// --- Phase 6: one serialized parser decl (design 2026-07-05) ------------------
// The forest is the immutable Tree-1 ROM; on bind the parser's symbol tables are
// RECONSTRUCTED from these records (never re-parsed). Slice 1 covers file-scope
// typedefs; kind widens to struct/class/func/template. A type reference is an
// madc type-id: primitive ids resolve everywhere (pinned); the forest's own
// aggregate types get system-segment ids (a later slice).
// --- Phase 6: complete type-table serialization (the "table segments" of the
// 2026-06-12 type-table design §6.4; §2 system segment "owned by the embedded
// forest"). The freeze serializes each project/system DataDef's FULL content,
// not just its name. Pointer fields ride as IDs and SWIZZLE back to pointers on
// load — the same move B1 made for cir_node (DataDef* -> typeid, char* -> intern
// handle), now applied to the DataDef graph itself. A DataDef's contiguous
// member/base vectors serialize directly once their element pointers are ids.
// This REPLACES the typeid->name-only closure and RETIRES the parallel
// decl_record/struct_member streams (no-parallel-implementations). ---

// Which DataDef the record reconstructs (its subclass), so load allocates the
// right object and reads the matching payload. Append-only (ABI-pinned by the
// context hash like the primitive slots).
enum : uint32_t
{
	CIR_TYPEK_OTHER     = 0,	// opaque: name-only (no reconstructable content)
	CIR_TYPEK_TYPEDEF   = 1,	// alias -> ref0 = underlying typeid
	CIR_TYPEK_STRUCT    = 2,	// DataDefSTRUCT: members[]
	CIR_TYPEK_UNION     = 3,	// DataDefSTRUCT union_layout: members[]
	CIR_TYPEK_CLASS     = 4,	// DataDefCLASS: members[] + bases[] (+ vtable meta)
	// Derived types — the SAME "table entry, pointer field as an id, swizzle on
	// load" shape as a typedef: ref0 = the operand's typeid, no member/base payload.
	// Load reconstructs via new DataDefPTR/REF/CONST(operand) in a fixpoint
	// (operand-before-derived; handles chains T** and self-referential Node*).
	CIR_TYPEK_POINTER   = 5,	// DataDefPTR:   ref0 = pointee typeid
	CIR_TYPEK_REFERENCE = 6,	// DataDefREF:   ref0 = referee typeid
	CIR_TYPEK_CONST     = 7		// DataDefCONST: ref0 = unqualified typeid
};

// Verbatim-layout flags on a type record (loaded as-is; no re-derivation).
enum : uint32_t
{
	CIR_TYPEF_UNION      = 1u << 0,	// union_layout
	CIR_TYPEF_COMPLETE   = 1u << 1,	// is_complete (a full body was parsed)
	CIR_TYPEF_SYSHDR     = 1u << 2,	// DataDefCLASS::from_system_header
	CIR_TYPEF_HAS_VTABLE = 1u << 3,	// DataDefCLASS::has_vtable
	CIR_TYPEF_HAS_VPTR   = 1u << 4,	// DataDefCLASS::has_vptr_slot
	CIR_TYPEF_USER_CTOR  = 1u << 5,	// DataDefCLASS::has_user_ctor
	CIR_TYPEF_USER_DTOR  = 1u << 6,	// DataDefCLASS::has_user_dtor
	CIR_TYPEF_ANON       = 1u << 7,	// DataDefSTRUCT::is_anonymous (tagless struct/union)
	CIR_TYPEF_REVERSE    = 1u << 8,	// DataDefSTRUCT::reverse_scalar_storage
	CIR_TYPEF_HAS_ANONAGG = 1u << 9	// DataDefSTRUCT::has_anon_aggregate (has re-nested anon groups)
};

// One serialized type-table entry (fixed 15 u32). name_id / spelling_id are
// intern handles; ref0 and the member/base payload type refs are typeids
// swizzled to DataDef* on load. member_begin/base_begin/method_begin index the
// container's type_payload u32 stream (raw u32 offsets); *_count = number of
// records there.
struct cir_forest_type_record
{
	uint32_t type_id;	// this DataDef's typeid (its serialization identity)
	uint32_t kind;		// CIR_TYPEK_*
	uint32_t name_id;	// interned name
	uint32_t spelling_id;	// interned canonical_cpp_spelling (0 = none)
	uint32_t size;		// byte size (verbatim)
	uint32_t align;		// alignment (verbatim)
	uint32_t nvsize;	// CLASS non-virtual size (DataDefCLASS::nvsize, verbatim) — drives
				// a functional-construction temp's alloca + the struct-copy size;
				// left behind before v16, so an empty tag class restored nvsize=0
				// and its `T x{}` / `T x = T()` global init emitted no copy/alloca

	uint32_t flags;		// CIR_TYPEF_*
	uint32_t ref0;		// CIR_TYPEK_TYPEDEF: underlying typeid; else 0
	uint32_t member_begin;	// u32 offset into type_payload for members
	uint32_t member_count;	// number of member records
	uint32_t base_begin;	// u32 offset into type_payload for bases (CLASS)
	uint32_t base_count;	// number of base records (CLASS)
	uint32_t method_begin;	// u32 offset into type_payload for methods (CLASS)
	uint32_t method_count;	// number of method records (CLASS)
	uint32_t namespace_id;	// interned defining namespace (0 = global); load
				// registers a namespaced type into namespace_map +
				// namespace_datatype_map, not just the flat maps
	uint32_t pack;		// DataDefSTRUCT::pack (0 = natural, 1 = packed, N = max align N)
	uint32_t tag_align;	// DataDefSTRUCT::tag_explicit_align (__attribute__((aligned(N))); 0 = none)
	uint32_t anon_begin;	// u32 offset into type_payload for anon-aggregate groups
	uint32_t anon_count;	// number of cir_forest_type_anon records (0 = none)
};

// One re-nested anonymous aggregate group in the type_payload stream (fixed
// 4-u32 stride). addAnonymousAggregate flattens an anon union/struct's members
// into the parent for name lookup AND retains this grouping so emission can
// re-nest a real `union{..}`/`struct{..}` — WITHOUT it, c2mir re-lays-out the
// flat members and the overlap is lost (a silent miscompile). aggregate_type_id
// is the nameless sub-aggregate's typeid, swizzled to its restored DataDefSTRUCT*.
struct cir_forest_type_anon
{
	uint32_t first_member;		// index of the group's first flattened member
	uint32_t member_count;		// number of flattened members in the group
	uint32_t offset;		// AnonymousAggregateInfo::offset (verbatim)
	uint32_t aggregate_type_id;	// the sub-aggregate's typeid (swizzle to DataDefSTRUCT*)
};

// A struct/class data member in the type_payload stream (fixed 11-u32 stride).
// Everything is loaded VERBATIM — the offset is the computed one, never a
// finalize()/compute_layout re-run. Bitfield fields are 0 for a normal member.
struct cir_forest_type_member
{
	uint32_t name_id;	// interned member name
	uint32_t type_id;	// member type (swizzle to DataDef*)
	uint32_t offset;	// member_offsets[i] (verbatim)
	uint32_t count;		// member_counts[i] (fixed-array count; 1 scalar)
	uint32_t access;	// member_access[i] (0=public / vfPRIVATE / vfPROTECTED)
	int32_t  origin;	// member_origin[i] (base index, or -1 = own)
	uint32_t bf_flags;	// bit0 is_bitfield | bit1 is_unsigned | bit2 reverse
	uint32_t bf_bit_offset;
	uint32_t bf_bit_width;
	uint32_t bf_storage_offset;
	uint32_t bf_storage_size;
};

// A direct base of a class in the type_payload stream (fixed 4-u32 stride).
struct cir_forest_type_base
{
	uint32_t base_type_id;	// the base's typeid (swizzle to DataDefCLASS*)
	uint32_t offset;	// BaseSpec.offset (verbatim)
	uint32_t flags;		// bit0 is_virtual | bit1 is_primary
	uint32_t access;	// BaseSpec.access
};

// Verbatim-layout flags on a method record.
enum : uint32_t
{
	CIR_METHF_CONST      = 1u << 0,	// FuncDef::is_const_method
	CIR_METHF_VARARGS    = 1u << 1,	// FuncDef::is_varargs
	CIR_METHF_VOIDPARAMS = 1u << 2,	// FuncDef::is_void_params (explicit `(void)`)
	CIR_METHF_STATIC     = 1u << 3,	// static member (no hidden __this)
	CIR_METHF_HAS_BODY   = 1u << 4,	// INLINE method: body_unit/body_idx locate its
					// Tree-1 func-def subtree in the AST (copied into
					// the consumer's Tree-2 on use). Absent => LIBRARY
					// method (body in a .so): declaration-only + emit_symbol.
	CIR_METHF_CTOR       = 1u << 5,	// constructor: load ALSO attaches it to cdd->ctors
					// (select_ctor_overload reads that set). A concrete
					// ctor has no method_map key (display_id 0) — it
					// resolves by ctor overload, not by name.
	CIR_METHF_DTOR       = 1u << 6	// destructor: display_id carries the "~"-prefixed
					// method_map key class_own_dtor scans for (the
					// FuncDef's own method_display_name is empty and is
					// used ONLY as that key), so the cleanup attribute
					// resolves the dtor symbol on the bound class.
};

// A non-virtual class method DECLARATION in the type_payload stream (fixed 7-u32
// stride, followed by param_count explicit-param typeids). The body is NOT here:
// an inline method's body rides the grove's node tree (emitted by the producer);
// an external/system method binds to emit_symbol. On load a FuncDef + Variable is
// rebuilt and attached to the class's method_map/methods so a member call
// resolves and links. The hidden __this (param 0 of a non-static method) is NOT
// serialized — it is rebuilt as a pointer to the owning class. ret_type_id and
// each explicit-param typeid swizzle to DataDef* (primitive or recorded aggregate;
// a method with an unserializable param/return is skipped individually).
struct cir_forest_type_method
{
	uint32_t name_id;	// Variable name = the mangled call symbol (Counter__get)
	uint32_t display_id;	// FuncDef::method_display_name (get)
	uint32_t ret_type_id;	// return type (swizzle)
	uint32_t emit_symbol_id; // FuncDef::emit_symbol intern (0 = madc-emitted default scheme)
	uint32_t flags;		// CIR_METHF_*
	uint32_t param_begin;	// u32 offset into type_payload for explicit param typeids
	uint32_t param_count;	// number of EXPLICIT params (excludes the hidden __this)
	uint32_t body_unit;	// CIR_METHF_HAS_BODY: unit of the method's Tree-1 func-def
	uint32_t body_idx;	// CIR_METHF_HAS_BODY: record idx of that func-def (else 0)
};

// A file-scope global VARIABLE definition (v13; container-global, fixed 3-u32
// stride). The forest serializes TYPES; a header's file-scope globals are a
// separate category the type pass does not cover, so binding <string> used to
// omit its inline globals (in_place, piecewise_construct, …) and the
// __madc_global_init that runs their ctors. This records the Variable's identity
// so load rebuilds it into tkProgram->variables + a dkGlobalVar TopDecl, and the
// EXISTING emission passes (the dkGlobalVar storage pass + collect_global_ctors +
// the __madc_global_init synthesis) emit it exactly as a live parse would — no
// new emission logic. v13 covers CLASS-typed globals: no initializer is stored
// because collect_global_ctors synthesizes the default ctor from the class's
// restored ctor set (Phase 6 v12). v14 adds SCALAR-const globals: a scalar's init
// is a compile-time constant (no ctor), so its integer value IS stored (init_value,
// valid iff CIR_GLOBALF_SCALAR_INIT) and load rebuilds a `T name = value;` decl.
enum : uint32_t {
	CIR_GLOBALF_SCALAR_INIT      = 1u << 0,	// init_value holds a scalar integer initializer (v14)
	CIR_GLOBALF_CLASS_VALUE_INIT = 1u << 1,	// v16: class value-init `T x{}` -> a trivially-copyable
						// self-copy via try_implicit_copy_construct (NO ctor needed)
	CIR_GLOBALF_CLASS_COPY_TEMP  = 1u << 2	// v16: class copy-init `T x = T()` -> construct a default
						// temporary (needs the class's default ctor) and copy it in
};
struct cir_forest_global_record
{
	uint32_t name_id;	// the Variable's name (intern handle)
	uint32_t type_id;	// its type (swizzle to DataDef*; a recorded aggregate or pinned scalar)
	uint32_t flags;		// Variable::flags verbatim (vfSTATIC/… — collect_global_ctors
				// and the dkGlobalVar pass read vfLOCAL/vfSTATIC/vfEXTERN)
	uint32_t gflags;	// forest-global flags (CIR_GLOBALF_*) — v14
	int64_t  init_value;	// scalar integer init (valid iff CIR_GLOBALF_SCALAR_INIT) — v14
};

// Source-position side-car record (parallel to the unit's records; cold —
// consumed for diagnostics, separate from the hot record segment).
struct cir_frozen_pos
{
	uint32_t fname_id;	// pool handle (0 = no position)
	uint32_t line;
	uint32_t col;
};

// One unit's freeze product. The v2 grove payload fields stay empty for a
// module-only freeze (anchor_idx then writes as ANCHOR_NONE).
struct cir_forest_unit
{
	uint32_t                    unit_name_id;
	cir_frozen_blob             blob;
	std::vector<uint64_t>       connectors;	// (target_unit << 32) | target_record
	std::vector<cir_frozen_pos> positions;	// parallel to blob.records
	// --- grove payload v2 (B4a) ---
	std::vector<uint8_t>        token_payload;	// .madh record form
	uint32_t                    token_count = 0;
	std::vector<cir_forest_decl_entry> decl_index;
	std::vector<uint32_t>       pp_events;	// cir_forest_pp_event stream
	std::vector<uint32_t>       edges;	// directory unit indices, include order
};

// A whole frozen forest in memory (the multi-unit sibling of cir_frozen_blob).
struct cir_frozen_forest
{
	std::vector<cir_forest_unit> units;
	uint32_t                     root_unit;
	uint32_t                     root_idx;
	std::vector<std::string>     libs;	// dlopen closure (#load / -l paths)
	// --- grove payload v2 (B4a; container-global) ---
	std::vector<uint32_t>        branch_macros;	// pool name ids, sorted
	std::vector<uint32_t>        canon_order;	// unit indices, canonical include order
	// --- Phase 6 (v6; container-global): complete type-table serialization.
	// One record per project/system DataDef (full content, pointer fields as
	// ids); member/base sub-records live in the type_payload u32 stream, sliced
	// per record by member_begin/base_begin. Load swizzles ids -> DataDef*. ---
	std::vector<cir_forest_type_record> type_records;
	std::vector<uint32_t> type_payload;
	// --- v13 (container-global): file-scope global VARIABLE definitions ---
	std::vector<cir_forest_global_record> globals;
	// --- B3 flip (container-global): the DefArena dump. Populated in madc_cir_freeze
	// from Program::forest_arena (filled during parse by the write-throughs). SAVE side
	// only for now — dumped alongside type_records, not yet read on load. ---
	madc::dis::DefArena arena;
	// Transient freeze-time helper (NOT serialized): every N_FUNC_DEF node's emit
	// symbol -> its (unit, record-idx) in the partitioned AST. cir_forest_append_methods
	// looks a method's mangled symbol up here to record where its INLINE body lives
	// (a symbol with no func-def in the AST is a LIBRARY method: no body location).
	std::map<std::string, std::pair<uint32_t, uint32_t> > funcdef_locs;
};

// The context-hash pin: madc version + record/position layout + the c2mir
// node-code enum tail + the typeid primitive tail. Stamped by writers,
// REQUIRED equal by CirFrozenForest::open (reject-and-fail, never mis-thaw).
uint64_t madc_cir_context_hash();

// Partition the sub-DAG rooted at `root` into per-unit segments keyed by
// each node's origin-token source file (origin-less nodes inherit their
// discovering parent's unit; the root falls back to `main_unit_name`).
// Interns unit names, string payloads, and position file names into the
// ACTIVE string pool — serialize that pool into the same container after
// this call. False on a null root or an out-of-format tree.
bool cir_freeze_forest(cir_node *root, const char *main_unit_name,
		       cir_frozen_forest &out);

// Stage a complete forest into a container: directory, string-pool blocks
// (the active pool, whose ids all forest handles reference), typeid->name
// closure, and every unit's four payload segments. The caller still sets
// the context hash (cir_forest_write does it) and picks placement
// (write_file / append_file / build).
bool cir_forest_write(const cir_frozen_forest &f, madc::dis::snapshot_writer &w,
		      PchCompression codec = PchCompression::Zlib);

// Map a container image for reading: the file at `path`, or the running
// executable (readlink /proc/self/exe) when `path` is NULL — the appended-
// blob placement. The mapping is never unmapped (thawed segments read from
// it for the process lifetime). False = no file / no blob.
bool cir_forest_map_image(const char *path, const void *&image, size_t &len);

class CirFrozenForest;

// A loaded frozen segment: joins the live (seg, idx) id space via the B1
// registry and materializes records to real cir_nodes on touch, at the
// c2mir edge (needs the c2m context for uids / uniq strings / positions).
// Owns both the records and the materialized node storage. Standalone mode
// (B2, no forest) resolves strings/positions against the LIVE substrate;
// as a forest unit it resolves against the container's own closure.
class CirFrozenSegment : public cir_segment_source
{
	friend class CirFrozenForest;

	cir_frozen_blob _blob;
	std::vector<uint64_t> _connectors;	// forest units only
	std::vector<cir_frozen_pos> _positions;	// forest units only
	CirFrozenForest *_forest;		// NULL = standalone (B2 mode)
	c2m_ctx_t _c2m;
	uint32_t _seg;				// registered segment id
	std::vector<cir_node *> _mat;		// per-record memo (NULL = cold)
	std::deque<cir_node> _nodes;		// materialized node storage (stable)

	cir_node *shell(uint32_t idx);		// phase A: node without children

	// Shared resolve driver (standalone + forest): iterative shell pass
	// across units (a connector to a cold unit loads it), then the child
	// appends. Defined once — CirFrozenSegment::node_at and
	// CirFrozenForest::node_for both enter here.
	static cir_node *resolve(CirFrozenSegment *seg, uint32_t idx);

public:
	CirFrozenSegment(cir_frozen_blob &&blob, c2m_ctx_t c2m);
	CirFrozenSegment(cir_forest_unit &&unit, CirFrozenForest *forest,
			 c2m_ctx_t c2m);
	~CirFrozenSegment();

	uint32_t seg() const { return _seg; }
	size_t record_count() const { return _blob.records.size(); }
	size_t materialized_count() const;
	CirFrozenForest *forest() const { return _forest; }
	uint64_t connector(uint32_t i) const { return _connectors[i]; }

	// THE resolve-on-touch entry: materialize the sub-DAG rooted at idx
	// (memoized; shares/cycles terminate) and return its real node.
	virtual cir_node *node_at(uint32_t idx);
};

class DataDef;	// defined in datadef.h; cir_freeze.cpp (the only .cpp that
		// materializes DataDefs) includes datadef.h before this header.
class Variable;	// datatokens.h; cir_freeze.cpp reconstructs method Variables (Phase 6 3d)

// One entry the bind layer registers into the parser's symbol tables. `dd` is a
// materialized struct/class DataDef (forest-owned); a typedef carries `underlying`
// instead and `dd == NULL`. `name` is the exported (unqualified) type name.
struct CirRestoredType
{
	const char *name;
	uint32_t    kind;		// CIR_TYPEK_*
	DataDef    *dd;			// struct/class object (NULL for a pure typedef)
	DataDef    *underlying;		// typedef target (else NULL)
	const char *ns;			// defining namespace (NULL/"" = global); load
					// registers the type into that namespace's maps
};

// A restored file-scope global variable (v13): its type swizzled back to a
// DataDef*. forest_restore_decls rebuilds a Variable into tkProgram->variables +
// a dkGlobalVar TopDecl from this, so the existing emission passes emit the
// global's storage + queue its ctor into __madc_global_init.
struct CirRestoredGlobal
{
	const char *name;		// the Variable's name
	DataDef    *type;		// its type (a recorded aggregate or pinned scalar)
	uint32_t    flags;		// Variable::flags verbatim
	uint32_t    gflags;		// forest-global flags (CIR_GLOBALF_*) — v14
	int64_t     init_value;		// scalar integer init (valid iff CIR_GLOBALF_SCALAR_INIT) — v14
};

// A loaded forest: validates the pin + directory + string pool once, then
// loads UNITS ON DEMAND — a unit's records decompress the first time a
// connector (or node_for) touches it, never at open. The image must stay
// mapped for the forest's lifetime (cir_forest_map_image never unmaps).
class CirFrozenForest
{
	friend class CirFrozenSegment;

	madc::dis::snapshot_reader _reader;
	c2m_ctx_t _c2m;
	std::vector<cir_forest_dir_unit> _units;
	std::vector<CirFrozenSegment *> _segs;	// lazily constructed per unit
	std::vector<std::string> _libs;
	std::map<uint32_t, const char *> _type_names;	// typeid -> pool c_str
	std::map<uint32_t, uint32_t> _live_ids;	// frozen str id -> live pool id
	std::map<std::string, uint32_t> _unit_by_name;	// unit-name spelling -> index (Phase 6 bind)
	uint32_t _root_unit, _root_idx;

	// The container's own string pool (A1 frozen view over the three
	// blocks; decompressed copies owned here when the segments are
	// compressed, bound in place when codec is None).
	madc::dis::frozen_intern_table _pool;
	std::vector<uint8_t> _pool_bytes, _pool_entries, _pool_buckets;
	// v2 container-global payloads (loaded at open; empty on v2-less
	// module containers).
	std::vector<uint32_t> _branch_macros, _canon_order;
	// v6 container-global: the serialized type table (Phase 6). Records +
	// payload load at open(); the DataDef objects materialize lazily at bind
	// (materialize_types), swizzling id refs back to pointers. The forest owns
	// the materialized objects and frees them in ~CirFrozenForest.
	std::vector<cir_forest_type_record> _type_records;
	std::vector<uint32_t> _type_payload;
	std::vector<DataDef *> _mat_storage;		// forest-owned materialized DataDefs
	std::vector<Variable *> _mat_vars;		// forest-owned reconstructed method Variables
	std::vector<CirRestoredType> _restored;		// bind-facing view (built once)
	// v13 container-global: file-scope global var defs. Records load at open();
	// _restored_globals (type-ids swizzled to DataDef*) is built by materialize_types.
	std::vector<cir_forest_global_record> _globals;
	std::vector<CirRestoredGlobal> _restored_globals;
	// v17 container-global: the B3 DefArena dump (segments 10-14), bound read-only
	// at open — in place when a segment is uncompressed, else into the owned
	// buffers. An arena-less freeze binds an empty view (zero-length segments).
	madc::dis::FrozenDefArena _arena;
	std::vector<uint8_t> _arena_defs, _arena_payload;
	std::vector<uint8_t> _arena_sbytes, _arena_sentries, _arena_sbuckets;
	bool _types_materialized;
	// Shared v2 segment reader: decompress unit slot `slot` into `out`
	// (raw bytes). False on absent/malformed.
	bool read_unit_seg(uint32_t unit, uint32_t slot, uint32_t kind,
			   std::vector<uint8_t> &out) const;

	const char *pool_cstr(uint32_t id, uint32_t &len) const;
	uint32_t live_str_id(uint32_t frozen_id);	// re-intern (memoized)

public:
	CirFrozenForest();
	~CirFrozenForest();

	// The load-on-demand step: a unit's records decompress + register on
	// FIRST touch (connector resolution enters here; B4 grove binding
	// will too). NULL on a corrupt/missing unit.
	CirFrozenSegment *unit_segment(uint32_t unit);

	// Validates the container, the context-hash pin, the directory, and
	// the string pool; reads libs + type names. Loads NO unit records.
	// On failure prints the reason to stderr and returns false.
	bool open(const void *image, size_t len, c2m_ctx_t c2m);

	// Rebind the c2m used to materialize nodes. The parse-time bind forest is
	// opened with c2m=NULL (it restores only types/PP, never node segments), so
	// the session c2m is set here before the first node materialization (inline
	// method body load at emit time). Safe iff no segment has materialized yet.
	void set_c2m(c2m_ctx_t c2m) { _c2m = c2m; }

	uint32_t unit_count() const { return (uint32_t)_units.size(); }
	size_t units_loaded() const;			// laziness observability
	const std::vector<std::string> &libs() const { return _libs; }
	const char *unit_name(uint32_t unit) const;
	// Reverse directory (Phase 6 bind): unit-name spelling -> unit index, or
	// -1 if no unit carries that name. Built once in open() over the directory.
	// The key is the exact unit_name string — a resolved include path or a bare
	// compiler-builtin/embedded name (e.g. "stddef.h").
	int find_unit(const std::string &name) const;
	const char *type_name_for(uint32_t type_id) const;  // NULL if unknown

	// --- grove payload v2 readers (B4a observability, B4b bind) ---
	// Each decompresses the requested unit segment on demand; false =
	// no payload (zero-length slot) or a malformed segment. They do NOT
	// load the unit's node records (independent of unit_segment).
	uint32_t unit_anchor(uint32_t unit) const;	// dir anchor_idx
	bool unit_tokens(uint32_t unit, std::vector<uint8_t> &madh_payload,
			 uint32_t &token_count);
	bool unit_decl_index(uint32_t unit,
			     std::vector<cir_forest_decl_entry> &out);
	bool unit_pp_events(uint32_t unit, std::vector<uint32_t> &out);
	bool unit_edges(uint32_t unit, std::vector<uint32_t> &out);
	const std::vector<uint32_t> &branch_macros() const { return _branch_macros; }
	const std::vector<uint32_t> &canon_order() const { return _canon_order; }
	// Phase 6: materialize the serialized type table into real DataDef objects
	// (idempotent; lazy — allocates on the first call). Swizzles member/base id
	// refs back to DataDef* (forest aggregates via the freeze-id map, primitives
	// via madc_type_from_id) and loads layout VERBATIM (no finalize / no
	// re-derivation). The bind layer registers the returned entries by name into
	// the parser's symbol tables. The forest owns the objects for its lifetime.
	const std::vector<CirRestoredType> &materialize_types();
	// v13: the restored file-scope globals (type-ids swizzled to DataDef*). Valid
	// after materialize_types() — call it first (it builds this view alongside the
	// types, reusing the same freeze-id -> DataDef* map).
	const std::vector<CirRestoredGlobal> &restored_globals() const
	{ return _restored_globals; }
	// v17: the read-only view over the dumped B3 DefArena (an empty view when the
	// container was frozen without the arena). Chunk 2's materialize_from_arena
	// reads this; until then it is the oracle surface verifying the SAVE dump.
	const madc::dis::FrozenDefArena &arena() const { return _arena; }
	// Container string pool lookup (name_id -> C string; NULL if invalid).
	const char *pool_str(uint32_t id) const
	{ uint32_t len; return pool_cstr(id, len); }

	// Resolve (unit, record) — the connector target form. Loads the unit
	// on first touch and materializes the reachable sub-DAG.
	cir_node *node_for(uint32_t unit, uint32_t idx);

	// The frozen tree's root.
	cir_node *root() { return node_for(_root_unit, _root_idx); }
};

// Structural identity oracle (forest Phase 2 gate): walk two trees in
// parallel — codes, payload class content, extension fields, child
// sequences — with the shared-subtree/cycle discipline of the dump walker.
// Returns false at the first divergence.
bool cir_trees_structurally_identical(node_t a, node_t b);

#endif // __CIR_FREEZE_H
