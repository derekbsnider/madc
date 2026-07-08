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
	SNAP_KIND_CIR_ARENA_PAYLOAD = madc::dis::SNAP_KIND_CONSUMER + 15, // container-global: uint32 arena payload stream
	// --- v20 (widening slice 2): template-NAME state — the parser's template
	// pattern maps (template_map / partial_spec_map / template_alias_map /
	// fn_template_map / fn_template_decl_map / var_template_map / concept_map).
	// Live keeps each pattern as captured TOKENS (the Borland model), so the
	// state serializes as token runs in the .madh record form + POD metadata. ---
	SNAP_KIND_CIR_TEMPLATES        = madc::dis::SNAP_KIND_CONSUMER + 16, // container-global: cir_forest_template_record[]
	SNAP_KIND_CIR_TEMPLATE_PAYLOAD = madc::dis::SNAP_KIND_CONSUMER + 17, // container-global: uint32 stream (params + token-run descriptors)
	SNAP_KIND_CIR_TEMPLATE_TOKENS  = madc::dis::SNAP_KIND_CONSUMER + 18, // container-global: raw token bytes (.madh record form)
	SNAP_KIND_CIR_EXTERN_LOCS      = madc::dis::SNAP_KIND_CONSUMER + 19, // container-global: cir_forest_extern_loc[] (extern decl index)
	// --- v23: the arena's raw token-byte block (DefArena::tokbytes, .madh
	// record form) — param-default expression runs referenced by BYTE offset
	// from paramrec.def_tok_off. ---
	SNAP_KIND_CIR_ARENA_TOKBYTES   = madc::dis::SNAP_KIND_CONSUMER + 20  // container-global: raw token bytes (.madh record form)
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
enum : uint32_t { CIR_FOREST_FORMAT_VERSION = 23 };	// v23: DEFAULT ARGUMENTS — a method/function parameter's default-argument expression serializes as its RAW SOURCE TOKEN run (FuncDef::param_default_tokens, captured at parseFunction's `= expr` branch during a --freeze parse; .madh record form in the new arena tokbytes block, segment 19) and paramrec grew a default-run reference (def_tok_off/bytes/count/file_id — a LAYOUT change: every params_begin offset shifts, so a stale v22 container mis-reads param runs). The pending-funcs flush deserializes each run and re-runs parseExpression over it (the ONE live derivation, inside the owner's class scope) to rebuild param_defaults — the state BOTH the arity gate (required_param_count) and the call-site default fill read; without it a bound `string greet = "hello"` found no matching basic_string ctor (the 2-param ctor's `= _Alloc()` default was not restored state). v22: MAP BURN-DOWN — (a) member-template methodrecs restore VERBATIM (the decl-only placeholders at their saved __oN ranks + the bodied instantiations, previously skipped by the v6 rule — a loaded _Rb_tree body calls pair(...)__oN directly, so without the def the MIR link died on an undefined import; the CIR_TMPLK_MEMBER flush now HYDRATES the restored placeholder's pattern fields instead of re-running the registration, which minted a second rank-shifted placeholder family); (b) cir_forest_global_record grew ns_id — a fresh instantiation in a consumer resolves a tag global by QUALIFIED name (std::piecewise_construct) through namespace_map[ns][name], so the flush reproduces live's namespace binding (a stale v21 container lacks the field and the member-template method records). v21: WIDENING SLICE 3 (part 1) — serialize the skipped-namespace-fn-template PLACEHOLDER surface. The RC2 free-function walk no longer skips declaration-only entries with a function_display_name (the __ns_std__Destroy-style placeholders register_skipped_namespace_template_function leaves in funcdef_map), and every DF_IS_FREE_FUNC record now carries the FuncDef-intrinsic state the live registration sets: function_display_name (disp_id), namespace_name (ns_id), inline_builtin_kind (defrec.builtin_kind_id) and the identity-return deduce pattern (tret_name_id / tret_arg_index / DF_TRET_* flags). Load rebuilds the placeholder FuncDef verbatim; the flush reproduces the live registration-site state — a first-wins namespace_map[ns][display] binding + the namespace_fn_overload_sets placeholder seed when the restored fn_template_map retains a body-bearing pattern for ns::display. Without this a NEW specialization in a consumer (vector<long> from a vector<int> producer) failed at parse: `std::_Destroy(...)` found no `_Destroy` in namespace std — the pattern TOKENS were restored (v20) but the resolution chokepoint, the placeholder Variable, was not. defrec grew 3 words (builtin_kind_id / tret_name_id / tret_arg_index — a stale v20 container lacks the placeholder records and the new fields). v20: WIDENING SLICE 2 — serialize the parser's TEMPLATE-NAME state (template_map / partial_spec_map / template_alias_map / fn_template_map / fn_template_decl_map / var_template_map / concept_map) so a bound header's template names RESOLVE in the consumer ("use of undeclared identifier 'vector'": the instantiation PRODUCT class was in the arena but the template NAME was not). Each captured pattern serializes VERBATIM as its live state: params (name / is_type / is_pack / per-param default token runs), flags, defining namespace, owner class (type-id, swizzled at load), and the captured TOKEN runs (body / decl / init / target / constraint / per-slot spec patterns) in the SAME .madh token record form the B4a unit token slices use (madc_pch::serialize_token_seq) — live itself keeps patterns as tokens (the Borland model), so bind holding the same tokens IS state parity, NOT the retired B4b re-parse drift. Load restores the maps before the consumer parses; an exact-match use then memo-hits the restored product (datatype_map[registered_mangled]) and a NEW specialization instantiates through the UNCHANGED live machinery. Three new container-global segments (15/16/17: records / u32 param+run payload / raw token bytes); CIR_FOREST_SEG_UNIT_BASE moved 16 -> 24 to make room (the version pin rejects every older container, so the move is safe). v19: RC2 — the arena additionally carries file-scope FREE-FUNCTION declarations (DK_FUNC records flagged DF_IS_FREE_FUNC, name_id = the funcdef_map key), restored on bind as funcdef_map entries + program-scope Variables so a bound call resolves the real signature and its extern proto emits with the real return/param types instead of the dlsym implicit-variadic fallback (i64,... — a signed-int return read as a 64-bit long, the bsearch_skill_exact bug class). No layout change — bumped because the freeze CONTENT grew (a stale v18 container lacks the records and would silently keep the fallback). v18: THE FLIP (B3 Chunk 3) — the DefArena dump (segments 10-14) IS the type-graph serialization; the v6 hand-serialized type records (segments 5/8: cir_forest_type_record[] + the member/base/method u32 payload stream) are RETIRED and no longer written or read. Load reconstructs the DataDef graph from the arena (materialize_from_arena, applying the v6 save-side selection at load), the typeid->name closure derives from arena defrec.name_id, and globals (segment 9, unchanged) swizzle through the arena reconstruct. A v17 container still carries the v6 segments a v18 reader would ignore — rejected by the version pin like every older format. v17: additionally dump the B3 DefArena (defrec[] + payload u32[] + its intern strings) as arena segments 10-14 alongside the v6 type records — the flip's SAVE side; not yet read on load (bind still uses the v6 type records), so this is additive, but the version is bumped because the container CONTENT grew. v16: serialize a bound header's file-scope CLASS-typed global INITIALIZER FORM (cir_forest_global_record.gflags gains CIR_GLOBALF_CLASS_VALUE_INIT / CIR_GLOBALF_CLASS_COPY_TEMP) so load reconstructs a TokenDecl whose emission is byte-identical to a live parse. v13 stored no form -> flush set decl=NULL -> collect_global_ctors' built-in path default-constructed DIRECTLY on the global; a live parse instead runs the class-instance init path: `T x = T()` builds a stack temp via the default ctor then copies it in (COPY_TEMP), while `T x{}` is a trivially-copyable self-copy via try_implicit_copy_construct (VALUE_INIT — needs NO ctor, so in_place, whose ctor was never serialized, binds anyway instead of being dropped by the ctors.empty() guard). v16 ALSO serializes DataDefCLASS::nvsize (the non-virtual size, left behind before v16 — a restored class defaulted nvsize=0): a functional-construction temporary's alloca AND the struct-copy in try_implicit_copy_construct are sized by nvsize, so an empty tag class restored with nvsize=0 emitted its `T x{}` / `T x = T()` global init with NO stack temp (alloca 0) and NO copy loop, unlike live. Together these close the LAST whole-<string>-TU MADC_DUMP_MIR gap (the in_place data item + the piecewise/allocator init-SHAPE divergence, which the top-level item-set diff hid inside __madc_global_init's body). The cir_forest_type_record grew a uint32_t (nvsize) vs v15 (a stale v15 container lacks the form flags -> would re-introduce the direct-construct shape + drop in_place). v15: serialize a class's OWN destructor even when it has neither an external emit_symbol NOR an inline body the producer emitted (a bodyless/unreferenced inline dtor) — declaration-only (CIR_METHF_DTOR, no body). A live parse registers such a dtor (class_own_dtor non-NULL) so Pass 1.6 synthesizes NO Cls___dtor; dropping it from the freeze made the restored class look dtor-less, so a --forest-bind consumer SYNTHESIZED a spurious trivial Cls___dtor that a live compile never emits (the whole-<string>-TU "only in BIND" overshoot: _Save_errno/__new_allocator_*/allocator_*/wide-char basic_string dtors). No layout change vs v14 — bumped because the freeze CONTENT changed (a stale v14 container lacks these dtor records → would re-introduce the overshoot). v14: serialize a bound header's file-scope SCALAR-const global VARIABLE definitions (cir_forest_global_record gains gflags + init_value): a scalar global's init is a compile-time constant (no ctor), so its integer value is stored and load rebuilds a `T name = value;` dkGlobalVar decl — a --forest-bind consumer of <string> now emits its scalar tag globals (hardware_constructive/destructive_interference_size = 64) as data items, byte-identically to live (in_place's {} value-init remains a follow-on: it uses try_implicit_copy_construct, not a ctor); v13: serialize a bound header's file-scope CLASS-typed global VARIABLE definitions (cir_forest_global_record) so load rebuilds them into tkProgram->variables + dkGlobalVar TopDecls and the existing passes emit them + synthesize __madc_global_init (the default ctors come from the v12 ctor set) — a --forest-bind consumer of <string> now emits its inline tag globals (piecewise_construct/allocator_arg) + global-init, as a live parse does; v12: serialize a class's ctors/dtor/operators (CIR_METHF_CTOR/DTOR) instead of skipping them, so load rebuilds cdd->ctors (default-construction resolves) + the "~" dtor method_map key (scope-exit cleanup resolves the D1 symbol) + operator= overloads — a std::string consumer now BINDS and RUNS correctly (constructs, assigns, sizes, destroys; output == live == g++). Whole-<string>-TU MIR byte-identity is NOT yet reached (two separate whole-TU-emission gaps: header global-var defs + __madc_global_init are not serialized; the bind synth-dtor set exceeds live's reachable set) — each its own follow-on slice; v11: serialize a non-polymorphic aggregate's COMPLETE state — anonymous_aggregates (re-nested nameless sub-aggregates, so a struct with an anon union/struct member binds with the right layout) + the layout scalars pack/tag_explicit_align/is_anonymous/reverse_scalar_storage/has_anon_aggregate — instead of a hand-picked field subset; v10: a type record carries its defining namespace (namespace_id) so load restores a namespaced type into namespace_map + namespace_datatype_map (a bound `N::P` / `std::X` resolves), not just the flat maps; v9: derived-type records (CIR_TYPEK_POINTER/REFERENCE/CONST, ref0 = operand typeid) so a pointer/reference/const member (or method param/return, or typedef underlying) serializes as a table entry + swizzles back on load, instead of bailing the whole aggregate; v8: an INLINE method carries its body location (body_unit/body_idx + CIR_METHF_HAS_BODY) so load reconnects the method to its Tree-1 func-def subtree (copied into the consumer's Tree-2 on use, like a template instantiation); a LIBRARY method has no func-def in the AST -> declaration-only + emit_symbol (unchanged); v7: class method declarations (non-virtual) ride the type record (method_begin/count + cir_forest_type_method); v6: complete type-table serialization (typeid->full DataDef, swizzle on load) replaces the typeid->name closure + the decl_record/struct_member parallel streams
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
	// seg ids 5 and 8 are RETIRED (the v6 type records / payload, deleted at the
	// v18 flip) — never reuse them for a new segment kind.
	CIR_FOREST_SEG_BRANCH_MACROS = 6,	// v2 (absent = zero-length)
	CIR_FOREST_SEG_CANON_ORDER   = 7,	// v2 (absent = zero-length)
	CIR_FOREST_SEG_GLOBALS       = 9,	// v13 (absent = zero-length): file-scope global var defs
	// B3: the DefArena dump (10-14) — THE type-graph serialization since v18.
	// Absent = zero-length (a type-less freeze).
	CIR_FOREST_SEG_ARENA_DEFS    = 10,	// madc::dis::defrec[] (id-addressed by project slot)
	CIR_FOREST_SEG_ARENA_PAYLOAD = 11,	// arena payload u32 stream
	CIR_FOREST_SEG_ARENA_STR_BYTES   = 12,	// arena intern bytes   (SNAP_KIND_INTERN_BYTES)
	CIR_FOREST_SEG_ARENA_STR_ENTRIES = 13,	// arena intern entries (SNAP_KIND_INTERN_ENTRIES)
	CIR_FOREST_SEG_ARENA_STR_BUCKETS = 14,	// arena intern buckets (SNAP_KIND_INTERN_BUCKETS)
	// v20 (widening slice 2): template-NAME state. Absent = zero-length (a
	// template-less freeze). UNIT_BASE moved 16 -> 24 in the same bump.
	CIR_FOREST_SEG_TEMPLATES        = 15,	// cir_forest_template_record[]
	CIR_FOREST_SEG_TEMPLATE_PAYLOAD = 16,	// uint32 stream: cir_forest_template_param[] + cir_forest_token_run[] slices
	CIR_FOREST_SEG_TEMPLATE_TOKENS  = 17,	// raw token bytes (.madh record form; runs carry byte offsets)
	CIR_FOREST_SEG_EXTERN_LOCS      = 18,	// cir_forest_extern_loc[] (extern decl index, v20)
	CIR_FOREST_SEG_ARENA_TOKBYTES   = 19,	// v23: DefArena::tokbytes (param-default token runs)
	CIR_FOREST_SEG_UNIT_BASE     = 24,
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

// --- Phase 6 / B3: the type graph rides the DefArena (cir_arena.h) -----------
// The forest is the immutable Tree-1 ROM; on bind the parser's symbol tables are
// RECONSTRUCTED from the dumped DefArena (never re-parsed). Since v18 the arena
// segments (10-14) ARE the type-graph serialization: parse-time write-throughs +
// the freeze-time refresh/completion passes populate one arena of POD records
// (defrec/memberrec/baserec/methodrec/paramrec/anonrec), cross-refs as type-ids
// (pinned primitive -> process global, project -> arena slot), and
// materialize_from_arena swizzles them back to DataDef objects on load. The v6
// hand-serialized record family (cir_forest_type_record + the member/base/method
// payload stream) is deleted (no-parallel-implementations). ---

// Kind tag on a RESTORED type surfaced to forest_restore_decls (CirRestoredType
// .kind) — the load-facing classification, mapped from the arena's DK_* kinds.
enum : uint32_t
{
	CIR_TYPEK_TYPEDEF   = 1,	// alias: underlying carries the target DataDef*
	CIR_TYPEK_STRUCT    = 2,	// DataDefSTRUCT
	CIR_TYPEK_UNION     = 3,	// DataDefSTRUCT union_layout
	CIR_TYPEK_CLASS     = 4,	// DataDefCLASS
	CIR_TYPEK_ENUM      = 5		// DataDefENUM (v21; scoped enumerators ride
					// CirRestoredType::enumerators)
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
	CIR_GLOBALF_CLASS_COPY_TEMP  = 1u << 2,	// v16: class copy-init `T x = T()` -> construct a default
						// temporary (needs the class's default ctor) and copy it in
	CIR_GLOBALF_EXTERN_REF       = 1u << 3	// v22: `extern T name;` REFERENCE to a library-defined
						// object (std::cout) — no storage, no ctor; the flush
						// rebuilds live's vfEXTERN Variable + Itanium
						// storage_alias_name (namespace_cpp_variable_symbol)
};
struct cir_forest_global_record
{
	uint32_t name_id;	// the Variable's name (intern handle)
	uint32_t type_id;	// its type (swizzle to DataDef*; a recorded aggregate or pinned scalar)
	uint32_t flags;		// Variable::flags verbatim (vfSTATIC/… — collect_global_ctors
				// and the dkGlobalVar pass read vfLOCAL/vfSTATIC/vfEXTERN)
	uint32_t gflags;	// forest-global flags (CIR_GLOBALF_*) — v14
	uint32_t ns_id;		// defining namespace (0 = global) — v22: a fresh
				// instantiation in a consumer resolves the tag by
				// QUALIFIED name (std::piecewise_construct) through
				// namespace_map[ns][name], which live's var-decl
				// inside `namespace std {}` registers
	int64_t  init_value;	// scalar integer init (valid iff CIR_GLOBALF_SCALAR_INIT) — v14
};

// --- v20 (widening slice 2): template-NAME state records -------------------
// One record per (map key, definition) pair across the parser's template
// pattern maps, serialized VERBATIM from the live map state at freeze. The
// captured token vectors ride the .madh token record form (serialize_token_seq)
// in the TEMPLATE_TOKENS segment; params + token-run descriptors ride the u32
// TEMPLATE_PAYLOAD stream (pod_append, the arena payload discipline).
enum : uint32_t {
	CIR_TMPLK_CLASS   = 1,	// Program::template_map (primary class templates)
	CIR_TMPLK_PARTIAL = 2,	// Program::partial_spec_map
	CIR_TMPLK_ALIAS   = 3,	// Program::template_alias_map
	CIR_TMPLK_FN      = 4,	// Program::fn_template_map
	CIR_TMPLK_FN_DECL = 5,	// Program::fn_template_decl_map (body-less decls)
	CIR_TMPLK_VAR     = 6,	// Program::var_template_map
	CIR_TMPLK_CONCEPT = 7,	// Program::concept_map
	CIR_TMPLK_MEMBER  = 8,	// a class's body-bearing MEMBER function template
				// (FuncDef::member_template_decl + owner) — restored
				// by re-running the live registration
				// (register_skipped_class_template_function) over the
				// restored tokens at flush time
	CIR_TMPLK_OUTOFLINE = 9	// Program::out_of_line_member_defs — an out-of-line
				// member DEFINITION of a class template
				// (vector.tcc's `template<..> RET vector<..>::f(..){..}`,
				// e.g. _M_realloc_insert). Key = "ns::Class"; OUTER
				// (class) params first, INNER (member-template) params
				// flagged CIR_TMPLP_IS_INNER; body run = the decl tokens.
};
enum : uint32_t {
	CIR_TMPLF_HAS_NON_TYPE_PARAMS = 1u << 0,	// TemplateDef/AliasDef/FnTemplateDef
	CIR_TMPLF_IS_PARTIAL_SPEC     = 1u << 1,	// TemplateDef::is_partial_specialization
	CIR_TMPLF_INSTANCE_METHOD     = 1u << 2,	// FnTemplateDef::instance_method
	CIR_TMPLF_OOL_MEMBER_TMPL     = 1u << 3		// OutOfLineMemberDef::is_member_template
};
enum : uint32_t {	// cir_forest_template_param::pflags
	CIR_TMPLP_IS_TYPE = 1u << 0,
	CIR_TMPLP_IS_PACK = 1u << 1,
	CIR_TMPLP_IS_INNER = 1u << 2	// OUTOFLINE: a MEMBER-template (inner) param
};
struct cir_forest_template_param
{
	uint32_t name_id;	// the type parameter's name (e.g. "_Tp")
	uint32_t pflags;	// CIR_TMPLP_*
};
// One captured token sequence: `tok_count` .madh-form records at byte offset
// `tok_off` (`tok_bytes` long) in the TEMPLATE_TOKENS segment. `file_id` is the
// sequence's origin file (its first token's), restored onto every token so
// instantiation provenance (_parse_file -> from_system_header classification,
// lazy-body deferral, error attribution) matches a live capture. The .madh
// record form keeps line/column per token but not file — one file per run is
// faithful in practice (a captured pattern's tokens come from one header).
struct cir_forest_token_run
{
	uint32_t tok_off;
	uint32_t tok_bytes;
	uint32_t tok_count;
	uint32_t file_id;	// 0 = none recorded
};
// The record's token runs are CONTIGUOUS cir_forest_token_run entries at
// `run_begin` (a u32 word offset in TEMPLATE_PAYLOAD), positional:
//   run[0]                       body (TemplateDef::body / FnTemplateDef::decl /
//                                VarTemplateDef::init / TemplateAliasDef::target)
//   run[1]                       constraint (TemplateDef / ConceptDef)
//   run[2 .. 2+param_count)      per-param default argument tokens
//   run[2+param_count ..
//        2+param_count+spec_count)  per-arg-slot partial-spec pattern tokens
// An absent sequence is a zero-count run. Total runs = 2 + param_count + spec_count.
struct cir_forest_template_record
{
	uint32_t kind;		// CIR_TMPLK_*
	uint32_t key_id;	// the map key (bare name, or "ns::name" for fn/var/concept maps)
	uint32_t name_id;	// TemplateDef::class_name / TemplateAliasDef::alias_name (0 = n/a)
	uint32_t ns_id;		// defining_namespace / FnTemplateDef::ns (0 = global)
	uint32_t extra_id;	// FnTemplateDef::inline_builtin_kind (0 = none)
	uint32_t owner_type_id;	// owner_class as a serialized type-id (0 = none)
	uint32_t flags;		// CIR_TMPLF_*
	uint32_t param_begin;	// u32 word offset of cir_forest_template_param[param_count]
	uint32_t param_count;
	uint32_t run_begin;	// u32 word offset of the positional token-run table
	uint32_t spec_count;	// partial-spec pattern slots (0 for a primary)
};

// v20: one indexed top-level EXTERN declaration — the producer's typed (or
// deliberately-unprototyped) extern decl for a symbol its lowering emitted
// (operator new/delete manglings, libc fns resolved via the dlsym fallback,
// the __madc_* runtime). A LOADED forest body carries pre-built calls to such
// symbols; the consumer loads the producer's OWN declaration node (verbatim
// state — never re-derives the signature) when one of them surfaces as a
// loaded-body callee with no in-TU definition.
struct cir_forest_extern_loc
{
	uint32_t name_id;	// the declared symbol (container pool)
	uint32_t unit;		// the N_SPEC_DECL's frozen location
	uint32_t idx;
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
	// --- v13 (container-global): file-scope global VARIABLE definitions ---
	std::vector<cir_forest_global_record> globals;
	// --- v20 (container-global): template-NAME state (widening slice 2) ---
	std::vector<cir_forest_template_record> templates;
	std::vector<uint32_t> template_payload;		// params + token-run descriptors (pod_append)
	std::vector<uint8_t>  template_tokens;		// .madh token record form (serialize_token_seq)
	// --- v20 (container-global): top-level extern-decl index (symbol -> loc) ---
	std::vector<cir_forest_extern_loc> extern_locs;
	// --- B3 (v18; container-global): the DefArena dump — THE type-graph
	// serialization. Populated in madc_cir_freeze from Program::forest_arena
	// (filled during parse by the write-throughs, refreshed + completed at
	// freeze). Load reconstructs DataDefs via materialize_from_arena. ---
	madc::dis::DefArena arena;
	// Transient freeze-time helper (NOT serialized): every N_FUNC_DEF node's emit
	// symbol -> its (unit, record-idx) in the partitioned AST. cir_forest_arena_complete
	// looks a method's mangled symbol up here to record where its INLINE body lives
	// (a symbol with no func-def in the AST is a LIBRARY method: no body location).
	std::map<std::string, std::pair<uint32_t, uint32_t> > funcdef_locs;
	// v21: each func-def's OWN source file (its origin token's file) — an
	// INSTANTIATED definition physically lands in the main-file unit but its
	// tokens carry the template's header origin; the body-stamping rule
	// classifies by this, falling back to the unit name (v20) when absent.
	std::map<std::string, const char *> funcdef_files;
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
class DataDefCLASS;	// datadef.h; v20 restored-template owner-class swizzle
class FuncDef;		// datadef.h; RC2 restored free-function declarations

// One entry the bind layer registers into the parser's symbol tables. `dd` is a
// materialized struct/class DataDef (forest-owned); a typedef carries `underlying`
// instead and `dd == NULL`. `name` is the exported (unqualified) type name.
struct CirRestoredType
{
	const char *name;
	uint32_t    kind;		// CIR_TYPEK_*
	DataDef    *dd;			// struct/class/enum object (NULL for a pure typedef)
	DataDef    *underlying;		// typedef target (else NULL)
	const char *ns;			// defining namespace (NULL/"" = global); load
					// registers the type into that namespace's maps
	// v21 (CIR_TYPEK_ENUM): the scoped enumerators (name, value) — load
	// rebuilds each as a constant Variable in the tag's pseudo-namespace,
	// exactly as TokenENUM::parse leaves them. Empty for other kinds and
	// for an enumerator-less opaque enum (std::align_val_t).
	std::vector<std::pair<const char *, int64_t> > enumerators;
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
	const char *ns;			// defining namespace (NULL/"" = global) — v22
	int64_t     init_value;		// scalar integer init (valid iff CIR_GLOBALF_SCALAR_INIT) — v14
};

// A restored file-scope FREE-FUNCTION declaration (RC2): the reconstructed
// FuncDef (forest-owned, declaration-only) under its call name.
// forest_restore_decls registers funcdef_map[name] + a program-scope Variable
// (deferred to the post-tkProgram flush, like a v13 global), so a bound call
// resolves the real signature and the extern proto emits with the real
// return/param types (Pass 0.75) — instead of the dlsym implicit-variadic
// fallback (`i64, ...`), which mis-reads a signed-int return as a 64-bit long.
struct CirRestoredFunc
{
	const char *name;		// the funcdef_map key == the call name
	FuncDef    *fd;			// reconstructed declaration (forest-owned)
};

// v20 (widening slice 2): one restored template pattern — the metadata view
// over a cir_forest_template_record with names resolved against the container
// pool, the owner class swizzled to its materialized DataDefCLASS*, and each
// token run exposed as a (bytes, len, count, file) span into the loaded
// TEMPLATE_TOKENS segment. forest_restore_decls (parser side, where the
// active string pool + Program::intern_file live) deserializes the runs via
// madc_pch::deserialize_tokens and registers the rebuilt definition into the
// matching live map — token materialization deliberately does NOT happen
// here (TokenIdent construction needs the active spelling pool).
struct CirRestoredTemplateRun
{
	const uint8_t *bytes;		// NULL/0-count = absent sequence
	uint32_t       len;
	uint32_t       count;
	const char    *file;		// origin file for token provenance (NULL = none)
};
struct CirRestoredTemplate
{
	uint32_t    kind;		// CIR_TMPLK_*
	const char *key;		// the map key
	const char *name;		// class_name / alias_name (NULL = n/a)
	const char *ns;			// defining namespace (NULL/"" = global)
	const char *extra;		// FnTemplateDef::inline_builtin_kind (NULL = none)
	DataDefCLASS *owner;		// swizzled owner class (NULL = none)
	uint32_t    flags;		// CIR_TMPLF_*
	std::vector<std::pair<const char *, uint32_t> > params;	// (name, CIR_TMPLP_*)
	CirRestoredTemplateRun body;
	CirRestoredTemplateRun constraint;
	std::vector<CirRestoredTemplateRun> defaults;	// parallel to params
	std::vector<CirRestoredTemplateRun> spec;	// per partial-spec arg slot
};

// v23: a restored FuncDef's param-DEFAULT token runs (paramrec.def_tok_* over
// the arena tokbytes block). param_defaults[i] is a PARSED TREE on the live
// side — not serializable — so the load carries the raw source tokens and the
// parser's pending-funcs flush re-runs parseExpression over each run (inside
// `owner`'s class scope for a method, so class-static names — npos — resolve
// as they did in the live parse). Index = the fd->parameters slot (hidden
// __this included, matching the live param_defaults alignment).
struct CirRestoredFuncDefaults
{
	FuncDef      *fd;		// the restored (forest-owned) FuncDef
	DataDefCLASS *owner;		// method owner (NULL = free function)
	const char   *ns;		// defining namespace (owner class's for a
					// method, namespace_name for a free fn;
					// NULL/"" = global) — the live parse ran
					// inside `namespace NS {}`, so unqualified
					// names (io_errc) resolve through it
	std::vector<std::pair<uint32_t, CirRestoredTemplateRun> > runs;	// (param index, token run)
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
	std::map<uint32_t, const char *> _type_names;	// typeid -> arena c_str (derived
							// from arena defrec.name_id at open)
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
	// The DataDef objects materialize lazily at bind (materialize_from_arena),
	// swizzling arena id refs back to pointers. The forest owns the
	// materialized objects and frees them in ~CirFrozenForest.
	std::vector<DataDef *> _mat_storage;		// forest-owned materialized DataDefs
	std::vector<Variable *> _mat_vars;		// forest-owned reconstructed method Variables
	std::vector<CirRestoredType> _restored;		// bind-facing view (built once)
	// v13 container-global: file-scope global var defs. Records load at open();
	// _restored_globals (type-ids swizzled to DataDef*) is built by
	// materialize_from_arena.
	std::vector<cir_forest_global_record> _globals;
	std::vector<CirRestoredGlobal> _restored_globals;
	// RC2: restored free-function declarations, built by materialize_from_arena
	// from the DF_IS_FREE_FUNC DK_FUNC records.
	std::vector<CirRestoredFunc> _restored_funcs;
	// v20 container-global: template-NAME state. Records + payload + token
	// bytes load at open(); _restored_templates (names resolved, owner
	// swizzled, runs exposed as spans) is built by materialize_from_arena.
	std::vector<cir_forest_template_record> _templates;
	std::vector<uint32_t> _template_payload;
	std::vector<uint8_t>  _template_tokens;
	std::vector<CirRestoredTemplate> _restored_templates;
	// v20 container-global: extern-decl index (symbol -> frozen location).
	std::map<std::string, std::pair<uint32_t, uint32_t> > _extern_by_name;
	// v18 container-global: the B3 DefArena dump (segments 10-14) — THE type-graph
	// serialization — bound read-only at open: in place when a segment is
	// uncompressed, else into the owned buffers. A type-less freeze binds an
	// empty view (zero-length segments).
	madc::dis::FrozenDefArena _arena;
	std::vector<uint8_t> _arena_defs, _arena_payload;
	std::vector<uint8_t> _arena_sbytes, _arena_sentries, _arena_sbuckets;
	std::vector<uint8_t> _arena_tokbytes;	// v23: param-default token runs
	// v23: per-FuncDef param-default token runs (paramrec.def_tok_*), built by
	// materialize_from_arena alongside the methods/free functions. The parser's
	// pending-funcs flush deserializes each run and re-runs parseExpression.
	std::vector<CirRestoredFuncDefaults> _restored_param_defaults;
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
	// B3 (v18): reconstruct the type graph from the dumped DefArena (idempotent;
	// lazy — allocates on the first call). Reads defrec/memberrec/baserec/
	// methodrec/anonrec/paramrec, applying at LOAD time the same selection rules
	// the retired v6 freeze applied at SAVE time (recordability closure; skip
	// polymorphic / vbase / union-layout classes, template methods, symbol-less
	// non-dtor specials) so the restored surface stays behavior-identical across
	// the flip — faithful widening past that selection is a follow-on, gated
	// against LIVE. Swizzles every id ref back to DataDef* (pinned primitives via
	// madc_type_from_id, project ids via the arena reconstruct) and loads layout
	// VERBATIM (no finalize / no re-derivation). The bind layer registers the
	// returned entries by name into the parser's symbol tables. The forest owns
	// the objects for its lifetime. Also builds the restored-globals view (the
	// CIR_GLOBALS records' type-ids swizzle through the same reconstruct).
	const std::vector<CirRestoredType> &materialize_from_arena();
	// v13: the restored file-scope globals (type-ids swizzled to DataDef*). Valid
	// after materialize_from_arena() — call it first (it builds this view
	// alongside the types, reusing the same arena-id -> DataDef* map).
	const std::vector<CirRestoredGlobal> &restored_globals() const
	{ return _restored_globals; }
	// RC2: the restored free-function declarations. Valid after
	// materialize_from_arena() — call it first.
	const std::vector<CirRestoredFunc> &restored_funcs() const
	{ return _restored_funcs; }
	// v20: the restored template-NAME state (metadata + token-run spans).
	// Valid after materialize_from_arena() — call it first.
	const std::vector<CirRestoredTemplate> &restored_templates() const
	{ return _restored_templates; }
	// v23: per-FuncDef param-default token runs. Valid after
	// materialize_from_arena() — call it first.
	const std::vector<CirRestoredFuncDefaults> &restored_param_defaults() const
	{ return _restored_param_defaults; }
	// v20: the frozen location of the producer's top-level extern decl for
	// `sym` (false = none indexed). The bind layer loads the decl node via
	// node_for when a loaded body references the symbol.
	bool extern_loc_for(const std::string &sym,
			    uint32_t &unit, uint32_t &idx) const
	{
		std::map<std::string, std::pair<uint32_t, uint32_t> >::const_iterator
			it = _extern_by_name.find(sym);
		if (it == _extern_by_name.end())
			return false;
		unit = it->second.first;
		idx  = it->second.second;
		return true;
	}
	// The read-only view over the dumped B3 DefArena (an empty view on a
	// type-less freeze).
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
