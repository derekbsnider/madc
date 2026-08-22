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

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "cir_node.h"
#include "madc_cir.h"		// cir_ledger_module — the decoded AOT-ledger form
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
	SNAP_KIND_CIR_ARENA_TOKBYTES   = madc::dis::SNAP_KIND_CONSUMER + 20, // container-global: raw token bytes (.madh record form)
	// --- MIR module cache (2026-07-17 design): the container's whole drained
	// module in MIR binary form (cir_forest_mir_header + MIR_write_module
	// bytes) — DERIVED state, regenerated per pack from the same compile
	// --run-frozen proves executable. OPTIONAL segment: absent = no cache,
	// consumers fall back to node materialization + c2mir bit-for-bit. ---
	SNAP_KIND_CIR_MIR_MODULE       = madc::dis::SNAP_KIND_CONSUMER + 21, // container-global: cir_forest_mir_header + .bmir module bytes
	// --- AOT ledger (forest-carriers S5): the C-lane madc runtime as MIR
	// modules, so a -static-libmadc emit merges the needed pieces into the
	// produced image. OPTIONAL segment: absent = no ledger (the flag then
	// refuses loudly). See cir_forest_ledger_header. ---
	SNAP_KIND_CIR_LEDGER           = madc::dis::SNAP_KIND_CONSUMER + 22, // container-global: cir_forest_ledger_header + entries + payload
	// --- v40: per-unit EXTERNAL branch dependencies (task #57) — the flat
	// u32 stream is [name_id, flags, value_id, definer_unit]... with flags
	// bit0 = defined-at-freeze, bit1 = value_id valid (object-like body),
	// definer_unit = directory index of the unit that established the
	// state (0xffffffff = none). Bind eligibility skips deps whose definer
	// is inside the closure being bound (replay-internal) and compares the
	// rest against the consumer's live macro tables; a mismatch declines
	// the whole root bind to live parse (or prunes an already-live unit
	// via its own guard). ---
	SNAP_KIND_CIR_BRANCH_DEPS      = madc::dis::SNAP_KIND_CONSUMER + 23, // per-unit: uint32 [name_id, flags, value_id, definer_unit] tuples
	// v41: exact source bytes before preprocessing. A config-mismatched
	// consumer may re-tokenize these without binding producer semantic state.
	SNAP_KIND_CIR_UNIT_SOURCE      = madc::dis::SNAP_KIND_CONSUMER + 24
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

// A frozen subtree in memory: record 0 is the root. decode_vector: the
// reader sizes these once and decodes over them — no zero-fill; the write
// side push_backs into them unchanged (allocator-transparent).
struct cir_frozen_blob
{
	madc::dis::decode_vector<cir_frozen_record> records;
	madc::dis::decode_vector<uint32_t>          children;
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

// CIR forest container format version. The version pin is absolute: a
// container whose version differs from the reader's is rejected wholesale
// (live parse takes over) — never partially read. History, newest first:
//
// v39: GLOBAL STORAGE ALIAS TRANSPORTED — cir_forest_global_record gains
// alias_id, Variable::storage_alias_name verbatim. The consumer used to
// RE-DERIVE the emitted symbol for every restored extern reference through the
// namespace-variable derivation, which is the wrong owner for a class-scope
// STATIC DATA MEMBER: its flat storage key `Tag__member` mangled as ONE
// identifier component (_ZSt14ctype_char__id) instead of the nested
// _ZNSt3__15ctypeIcE2idE that libc++/libstdc++ actually export, so
// `std::use_facet<F>` under a bound grove died on an undeclared identifier (or
// an undefined MIR import for the invented name). The producer already held the
// right name — every entity category derives its symbol through its OWN owner
// at parse — so it is carried, not recomputed. LOADED == parsed covers derived
// names too.
//
// v38: MEMBER-TEMPLATE PARAM CONSTRAINT RUNS — the ClassMethodPattern payload
// gains a per-param constraint-run section between the v36 defaults and the
// v37 function-param type runs, and CIR_TMPLK_MEMBER records carry the runs
// in the record's spec slot (the FN lane's v33 convention). The runs are a
// non-type param's compound declared TYPE (`typename enable_if<C,bool>::type`)
// and gate which non-type defaults the instantiation twins may fill: an EMPTY
// run (`bool _Dummy = true`, libc++'s unique_ptr(pointer, deleter) family)
// fills; a captured run still clears its default (the pair-ctor incident)
// until runs are evaluated at instantiation. Without the carriage a thawed
// member template would fill nothing (pre-v38 behavior) while the live parse
// fills — LOADED != parsed on the packed lane.
//
// v37: MEMBER-TEMPLATE FUNCTION-PARAM TYPE RUNS — the ClassMethodPattern
// payload gains a per-FUNCTION-parameter type-token-run section
// (FuncDef::member_template_param_type_tokens): the OTHER [temp.deduct]/8
// half, SFINAE carried in a parameter type (`typename
// _Up::iterator_category* = nullptr`, libc++'s __has_iterator_category
// __test pair). (This entry documents the bump retroactively — it landed
// without a history line.)
//
// v36: MEMBER-TEMPLATE PARAM DEFAULTS — the ClassMethodPattern payload gains
// a per-param default-token-run section after member_template_return_tokens,
// and CIR_TMPLK_MEMBER records now fill their (always-present, previously
// empty) per-param default runs. A member template can carry its whole
// [temp.deduct]/8 SFINAE in a defaulted param (`typename =
// decltype(declval<_Tp1&>().~_Tp1())`, gcc13 __is_destructible_impl::__test);
// without the runs a thawed candidate never fails substitution and
// is_destructible<X> answered 1 for a deleted destructor.
//
// v35: TEMPLATE-INSTANTIATION KEY P/R SPLIT — canonical_arg_key_fragment maps
// '*' -> 'P' and '&' -> 'R' (was both '_'), so X<int*> and X<int&> stop
// colliding into one registered instantiation (remove_reference<int*> served
// remove_reference<int&>'s cached class: std::move<int*>'s return referent
// read int32_t, one pointer level short). Instantiated-class registered names
// feed serialized symbols, so a v34 container's names mismatch a v35 reader's
// re-derivation — the version pin rejects it (live parse takes over).
// Also under v35 (landed one commit earlier in the same series): the
// ClassMethodPattern payload gains a noexcept_spec word after is_deleted,
// and DK_FUNC records carry DF_NOEXCEPT_TRUE / DF_NOEXCEPT_UNKNOWN flags.
//
// v34: DECL-ONLY MEMBER TEMPLATES — CIR_TMPLK_MEMBER records are also emitted
// for body-less member function templates (the __do_common_type_impl::_S_test
// SFINAE shape), carrying the dependent return-type token range
// (FuncDef::member_template_return_tokens) in the record's previously-empty
// constraint-run slot; the flush stamps the restored placeholder's pattern
// fields directly (no decl tokens exist to re-run the registration over). NO
// layout change, but a stale v33 container lacks the records: a thawed decl-
// only member template kept a bare placeholder, so decltype(_S_test<A,B>(0))
// fell to the implicit 64-bit return and common_type<A,B>'s base materialized
// as int64_t (LOADED != parsed, silent wrong answer — the packed-lane
// testcommontype failure) — the version pin rejects it. v34 also folds the
// stdlib FLAVOR's effective name into madc_forest_config_word (bits 17..31):
// a libstdc++-parsed container bound into a -stdlib=libc++ compile served
// the wrong <stddef.h> and tripped libc++'s <cstddef> #error (the packed-lane
// testcommontype_libcxx failure); a flavor mismatch now takes the v27 gate's
// silent live fall-through.
//
// v33: FN-TEMPLATE PER-PARAM CONSTRAINT RUNS — CIR_TMPLK_FN/FN_DECL records
// carry each template parameter's captured constraint-TYPE token run
// (FnTemplateDef::typeparam_constraints — the SFINAE overload-selection
// carrier evaluated at instantiation) in the record's previously-empty per-
// slot spec multi-run. NO layout change, but a stale v32 container lacks the
// runs, so a load would silently drop every SFINAE constraint and instantiate
// the FIRST overload of a constrained set (LOADED != parsed, silent wrong
// answer) — the version pin rejects it.
//
// v32: MEMBER VBASE PROVENANCE — memberrec grew vbase_id (the member-index ->
// hosting-VIRTUAL-BASE map DataDefSTRUCT::member_vbase, previously parse-time-
// only state): member access through a virtual-base VIEW reads it to pick the
// dynamic vtable-slot adjust, so without the field a restored header class
// silently degraded to the static offset (LOADED != parsed). A LAYOUT change
// (memberrec stride +1 word — every members_begin run in a stale container
// mis-reads); the version pin rejects v31 corpora.
//
// v31: CLASS-PATTERN semantic fingerprints hash concrete types by canonical
// SPELLING (process-stable) instead of the raw runtime type id (process-local
// — a correctly-swizzled restore could never verify, and the old reads were
// vacuously verifying dangling ids); the payload reader also swizzles
// concrete_type_id through the forest tid map, and the ClassParseReason wire
// vocabulary gains UnsupportedMemberTemplateOverloads. Stale v30 corpora carry
// id-hashed fingerprints every v31 read would reject — the version pin rejects
// them wholesale instead.
//
// v30: CLASS-PATTERN method parameters persist direct template-parameter
// spelling provenance; stale v29 readers must reject it.
//
// v29: CLASS-PATTERN B3 payload adds inherited-using recipes and nested
// class/alias-template recipes; stale v28 readers must reject it.
//
// v28: CLASS PATTERN PAYLOAD — CIR_TMPLK_CLASS/PARTIAL records carry a
// normalized ClassPattern slice in the existing TEMPLATE_PAYLOAD segment plus
// method/default token-run references in TEMPLATE_TOKENS; the record grew
// pattern_begin/pattern_words/pattern_reason, so v27 readers must reject it.
//
// v27: PRODUCER-CONFIG GATE — the directory header records the producer's
// language standard (Program::LanguageStd | gnu_dialect<<16) and a fold of its
// -D command-line defines (madc_forest_config_word /
// madc_forest_defines_hash); ensure_bind_forest requires BOTH equal to the
// consumer's before binding, else silent live fall-through. Needed by the
// packed-binary default: a C++-parsed corpus carries the REAL glibc paths
// (stdio.h et al.) as units, which a C compile's resolved #include would
// otherwise match — binding C++-parsed state into a C compile. The header grew
// two words (a LAYOUT change; the version pin rejects older containers).
//
// v26: DEFERRED METHOD BODIES (DK_DEFBODY) — a system-header method body the
// producer never ODR-used has no func-def in the frozen AST (only TRANSLATED
// defs freeze); live materializes it from TOKENS via parse_deferred_lazy_body
// on first use, so the map entry (Program::deferred_lazy_bodies: 4 token
// vectors + owner + full_definition + position) now serializes as DK_DEFBODY
// records (token runs in the arena tokbytes; no defrec layout change — per-
// kind field reuse) and the flush rebuilds each entry with var = the restored
// method Variable, letting the EXISTING m&l fixpoint re-run the one live
// derivation on first ODR-use. Without it a consumer's first use of a
// producer-unused inline method (basic_string's copy ctor, __gnu_cxx
// char_traits compare) died as an undefined MIR import.
//
// v25: C-ARRAY TYPES (DK_CARRAY) — a fixed-size C array type (DataDefCArray,
// e.g. va_list's `struct __madc_va_list_tag[1]` typedef underlying) records
// like the other derived types (ref0 = element type-id; the folded 64-bit
// count in the NEW defrec carray_count_lo/hi words — a LAYOUT change, so a
// stale v24 container mis-reads defrecs) and pass 1b rebuilds it operand-
// before-derived. Without it every array-underlying typedef "cleanly lacked"
// at the arena_complete resolvability check, so a bound <stdarg.h> had no
// va_list (soak family a, 9 tests) and every free fn with an array-typed param
// (vsprintf) kept the dlsym variadic fallback. A runtime-sized array
// (count_expr set) is function-local state and is still never recorded.
//
// v24: TU-ROOT ORIGIN FENCE (owner correction: the forest holds the #include
// files' state ONLY — never the program's). Every record whose DEFINING FILE
// is the TU's root source is stamped (defrec DF_TU_ROOT_ORIGIN — aggregates
// preserve the stamp across the freeze-time re-record; enums/typedefs derive
// from their registered token's file, free functions from FuncDef::decl_file;
// globals get CIR_GLOBALF_TU_ROOT from their TopDecl's token; template records
// get CIR_TMPLF_TU_ROOT from their pattern tokens' file) and the bind restore
// FENCES every stamped record out of its name-registration surfaces (_restored
// types/enums/typedefs, restored globals/free-fns/templates/param-defaults).
// The records STAY in the arena — --run-frozen's cross-process typeid->name
// closure reads them. The discriminator is ROOT-vs-INCLUDE, NOT system-vs-
// user: user headers (the bind gates' fbgate_*.h, testinclude's helper) keep
// restoring. Without the fence the per-file harness leaked the producer's OWN
// program state into the consumer (soak family 1, ~250 tests: "Struct already
// defined", class-token demotion, typedef/enum grammar desyncs, default-
// constructed own globals). A stale v23 container lacks the stamps — the
// version pin rejects it.
//
// v23: DEFAULT ARGUMENTS — a method/function parameter's default-argument
// expression serializes as its RAW SOURCE TOKEN run
// (FuncDef::param_default_tokens, captured at parseFunction's `= expr` branch
// during a --freeze parse; .madh record form in the new arena tokbytes block,
// segment 19) and paramrec grew a default-run reference
// (def_tok_off/bytes/count/file_id — a LAYOUT change: every params_begin
// offset shifts, so a stale v22 container mis-reads param runs). The pending-
// funcs flush deserializes each run and re-runs parseExpression over it (the
// ONE live derivation, inside the owner's class scope) to rebuild
// param_defaults — the state BOTH the arity gate (required_param_count) and
// the call-site default fill read; without it a bound `string greet = "hello"`
// found no matching basic_string ctor (the 2-param ctor's `= _Alloc()` default
// was not restored state).
//
// v22: MAP BURN-DOWN — (a) member-template methodrecs restore VERBATIM (the
// decl-only placeholders at their saved __oN ranks + the bodied
// instantiations, previously skipped by the v6 rule — a loaded _Rb_tree body
// calls pair(...)__oN directly, so without the def the MIR link died on an
// undefined import; the CIR_TMPLK_MEMBER flush now HYDRATES the restored
// placeholder's pattern fields instead of re-running the registration, which
// minted a second rank-shifted placeholder family); (b)
// cir_forest_global_record grew ns_id — a fresh instantiation in a consumer
// resolves a tag global by QUALIFIED name (std::piecewise_construct) through
// namespace_map[ns][name], so the flush reproduces live's namespace binding (a
// stale v21 container lacks the field and the member-template method records).
//
// v21: WIDENING SLICE 3 (part 1) — serialize the skipped-namespace-fn-template
// PLACEHOLDER surface. The RC2 free-function walk no longer skips declaration-
// only entries with a function_display_name (the __ns_std__Destroy-style
// placeholders register_skipped_namespace_template_function leaves in
// funcdef_map), and every DF_IS_FREE_FUNC record now carries the FuncDef-
// intrinsic state the live registration sets: function_display_name (disp_id),
// namespace_name (ns_id), inline_builtin_kind (defrec.builtin_kind_id) and the
// identity-return deduce pattern (tret_name_id / tret_arg_index / DF_TRET_*
// flags). Load rebuilds the placeholder FuncDef verbatim; the flush reproduces
// the live registration-site state — a first-wins namespace_map[ns][display]
// binding + the namespace_fn_overload_sets placeholder seed when the restored
// fn_template_map retains a body-bearing pattern for ns::display. Without this
// a NEW specialization in a consumer (vector<long> from a vector<int>
// producer) failed at parse: `std::_Destroy(...)` found no `_Destroy` in
// namespace std — the pattern TOKENS were restored (v20) but the resolution
// chokepoint, the placeholder Variable, was not. defrec grew 3 words
// (builtin_kind_id / tret_name_id / tret_arg_index — a stale v20 container
// lacks the placeholder records and the new fields).
//
// v20: WIDENING SLICE 2 — serialize the parser's TEMPLATE-NAME state
// (template_map / partial_spec_map / template_alias_map / fn_template_map /
// fn_template_decl_map / var_template_map / concept_map) so a bound header's
// template names RESOLVE in the consumer ("use of undeclared identifier
// 'vector'": the instantiation PRODUCT class was in the arena but the template
// NAME was not). Each captured pattern serializes VERBATIM as its live state:
// params (name / is_type / is_pack / per-param default token runs), flags,
// defining namespace, owner class (type-id, swizzled at load), and the
// captured TOKEN runs (body / decl / init / target / constraint / per-slot
// spec patterns) in the SAME .madh token record form the B4a unit token slices
// use (madc_pch::serialize_token_seq) — live itself keeps patterns as tokens
// (the Borland model), so bind holding the same tokens IS state parity, NOT
// the retired B4b re-parse drift. Load restores the maps before the consumer
// parses; an exact-match use then memo-hits the restored product
// (datatype_map[registered_mangled]) and a NEW specialization instantiates
// through the UNCHANGED live machinery. Three new container-global segments
// (15/16/17: records / u32 param+run payload / raw token bytes);
// CIR_FOREST_SEG_UNIT_BASE moved 16 -> 24 to make room (the version pin
// rejects every older container, so the move is safe).
//
// v19: RC2 — the arena additionally carries file-scope FREE-FUNCTION
// declarations (DK_FUNC records flagged DF_IS_FREE_FUNC, name_id = the
// funcdef_map key), restored on bind as funcdef_map entries + program-scope
// Variables so a bound call resolves the real signature and its extern proto
// emits with the real return/param types instead of the dlsym implicit-
// variadic fallback (i64,... — a signed-int return read as a 64-bit long, the
// bsearch_skill_exact bug class). No layout change — bumped because the freeze
// CONTENT grew (a stale v18 container lacks the records and would silently
// keep the fallback).
//
// v18: THE FLIP (B3 Chunk 3) — the DefArena dump (segments 10-14) IS the type-
// graph serialization; the v6 hand-serialized type records (segments 5/8:
// cir_forest_type_record[] + the member/base/method u32 payload stream) are
// RETIRED and no longer written or read. Load reconstructs the DataDef graph
// from the arena (materialize_from_arena, applying the v6 save-side selection
// at load), the typeid->name closure derives from arena defrec.name_id, and
// globals (segment 9, unchanged) swizzle through the arena reconstruct. A v17
// container still carries the v6 segments a v18 reader would ignore — rejected
// by the version pin like every older format.
//
// v17: additionally dump the B3 DefArena (defrec[] + payload u32[] + its
// intern strings) as arena segments 10-14 alongside the v6 type records — the
// flip's SAVE side; not yet read on load (bind still uses the v6 type
// records), so this is additive, but the version is bumped because the
// container CONTENT grew.
//
// v16: serialize a bound header's file-scope CLASS-typed global INITIALIZER
// FORM (cir_forest_global_record.gflags gains CIR_GLOBALF_CLASS_VALUE_INIT /
// CIR_GLOBALF_CLASS_COPY_TEMP) so load reconstructs a TokenDecl whose emission
// is byte-identical to a live parse. v13 stored no form -> flush set decl=NULL
// -> collect_global_ctors' built-in path default-constructed DIRECTLY on the
// global; a live parse instead runs the class-instance init path: `T x = T()`
// builds a stack temp via the default ctor then copies it in (COPY_TEMP),
// while `T x{}` is a trivially-copyable self-copy via
// try_implicit_copy_construct (VALUE_INIT — needs NO ctor, so in_place, whose
// ctor was never serialized, binds anyway instead of being dropped by the
// ctors.empty() guard). v16 ALSO serializes DataDefCLASS::nvsize (the non-
// virtual size, left behind before v16 — a restored class defaulted nvsize=0):
// a functional-construction temporary's alloca AND the struct-copy in
// try_implicit_copy_construct are sized by nvsize, so an empty tag class
// restored with nvsize=0 emitted its `T x{}` / `T x = T()` global init with NO
// stack temp (alloca 0) and NO copy loop, unlike live. Together these close
// the LAST whole-<string>-TU MADC_DUMP_MIR gap (the in_place data item + the
// piecewise/allocator init-SHAPE divergence, which the top-level item-set diff
// hid inside __madc_global_init's body). The cir_forest_type_record grew a
// uint32_t (nvsize) vs v15 (a stale v15 container lacks the form flags ->
// would re-introduce the direct-construct shape + drop in_place).
//
// v15: serialize a class's OWN destructor even when it has neither an external
// emit_symbol NOR an inline body the producer emitted (a bodyless/unreferenced
// inline dtor) — declaration-only (CIR_METHF_DTOR, no body). A live parse
// registers such a dtor (class_own_dtor non-NULL) so Pass 1.6 synthesizes NO
// Cls___dtor; dropping it from the freeze made the restored class look dtor-
// less, so a --forest-bind consumer SYNTHESIZED a spurious trivial Cls___dtor
// that a live compile never emits (the whole-<string>-TU "only in BIND"
// overshoot: _Save_errno/__new_allocator_*/allocator_*/wide-char basic_string
// dtors). No layout change vs v14 — bumped because the freeze CONTENT changed
// (a stale v14 container lacks these dtor records → would re-introduce the
// overshoot).
//
// v14: serialize a bound header's file-scope SCALAR-const global VARIABLE
// definitions (cir_forest_global_record gains gflags + init_value): a scalar
// global's init is a compile-time constant (no ctor), so its integer value is
// stored and load rebuilds a `T name = value;` dkGlobalVar decl — a --forest-
// bind consumer of <string> now emits its scalar tag globals
// (hardware_constructive/destructive_interference_size = 64) as data items,
// byte-identically to live (in_place's {} value-init remains a follow-on: it
// uses try_implicit_copy_construct, not a ctor);
//
// v13: serialize a bound header's file-scope CLASS-typed global VARIABLE
// definitions (cir_forest_global_record) so load rebuilds them into
// tkProgram->variables + dkGlobalVar TopDecls and the existing passes emit
// them + synthesize __madc_global_init (the default ctors come from the v12
// ctor set) — a --forest-bind consumer of <string> now emits its inline tag
// globals (piecewise_construct/allocator_arg) + global-init, as a live parse
// does;
//
// v12: serialize a class's ctors/dtor/operators (CIR_METHF_CTOR/DTOR) instead
// of skipping them, so load rebuilds cdd->ctors (default-construction
// resolves) + the "~" dtor method_map key (scope-exit cleanup resolves the D1
// symbol) + operator= overloads — a std::string consumer now BINDS and RUNS
// correctly (constructs, assigns, sizes, destroys; output == live == g++).
// Whole-<string>-TU MIR byte-identity is NOT yet reached (two separate whole-
// TU-emission gaps: header global-var defs + __madc_global_init are not
// serialized; the bind synth-dtor set exceeds live's reachable set) — each its
// own follow-on slice;
//
// v11: serialize a non-polymorphic aggregate's COMPLETE state —
// anonymous_aggregates (re-nested nameless sub-aggregates, so a struct with an
// anon union/struct member binds with the right layout) + the layout scalars p
// ack/tag_explicit_align/is_anonymous/reverse_scalar_storage/has_anon_aggregat
// e — instead of a hand-picked field subset;
//
// v10: a type record carries its defining namespace (namespace_id) so load
// restores a namespaced type into namespace_map + namespace_datatype_map (a
// bound `N::P` / `std::X` resolves), not just the flat maps;
//
// v9: derived-type records (CIR_TYPEK_POINTER/REFERENCE/CONST, ref0 = operand
// typeid) so a pointer/reference/const member (or method param/return, or
// typedef underlying) serializes as a table entry + swizzles back on load,
// instead of bailing the whole aggregate;
//
// v8: an INLINE method carries its body location (body_unit/body_idx +
// CIR_METHF_HAS_BODY) so load reconnects the method to its Tree-1 func-def
// subtree (copied into the consumer's Tree-2 on use, like a template
// instantiation); a LIBRARY method has no func-def in the AST -> declaration-
// only + emit_symbol (unchanged);
//
// v7: class method declarations (non-virtual) ride the type record
// (method_begin/count + cir_forest_type_method);
//
// v6: complete type-table serialization (typeid->full DataDef, swizzle on
// load) replaces the typeid->name closure + the decl_record/struct_member
// parallel streams
// v40: per-unit BRANCH DEPENDENCIES (slot +8; SEGS_PER_UNIT 8 -> 9): the
// macros a unit's PP conditionals consulted whose state was established
// OUTSIDE the unit's own top-level include closure at freeze (an earlier
// sibling's #define — mingw stdlib.h defining errno before errno.h parsed —
// or an undefined guard). Bind eligibility compares them against the
// consumer's live macro tables and DECLINES to live parse on mismatch:
// frozen unit state is order-conditional, and replaying it into a consumer
// whose environment took the other branch silently loses declarations
// (the win64 packed lane's errno/stod family, task #57).
//
// v41: per-unit RAW SOURCE (slot +9; SEGS_PER_UNIT 9 -> 10). The forest still
// binds semantic state only on an exact v27 config match; a compiler-less
// packaged target can instead tokenize the producer's exact header bytes
// under another --std=/-D/POSIX config. This is source fallback, never a
// relaxation of LOADED == parsed.
enum : uint32_t { CIR_FOREST_FORMAT_VERSION = 41 };
enum : uint32_t { CIR_FOREST_CONFIG_STDLIB_MASK = 0xfffe0000u };
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
	CIR_FOREST_SEG_MIR_MODULE       = 20,	// MIR module cache (optional; absent = no cache)
	CIR_FOREST_SEG_LEDGER           = 21,	// S5 AOT ledger (optional; absent = no ledger)
	CIR_FOREST_SEG_UNIT_BASE     = 24,
	CIR_FOREST_SEGS_PER_UNIT     = 10	// +0 records, +1 children, +2 connectors,
						// +3 positions, +4 tokens, +5 decl index,
						// +6 pp exports, +7 edges, +8 branch deps
						// +9 raw source (v41; slots may be zero-length)
};

struct cir_forest_dir_header	// directory payload: header, then units, then lib name ids
{
	uint32_t version;	// CIR_FOREST_FORMAT_VERSION
	uint32_t unit_count;
	uint32_t root_unit;	// the frozen tree's root record
	uint32_t root_idx;
	uint32_t lib_count;	// required dlopen()'d libraries (link-environment closure)
	uint32_t language_std;	// v27: producer config — Program::LanguageStd | (gnu_dialect << 16)
	uint32_t defines_hash;	// v27: fold of the producer's -D cli defines (0 = none)
	uint32_t _pad;
};

// MIR module cache segment header (precedes the raw MIR_write_module bytes).
// Coupling: the container version pin + context hash already reject a
// container from a different madc build; this stamp adds the MIR API level
// so a blob never feeds a MIR_read from an incompatible libmir (the .bmir
// stream's own internal version check is the final backstop, error-contained
// on the read side).
struct cir_forest_mir_header
{
	uint32_t forest_version;	// CIR_FOREST_FORMAT_VERSION at pack
	uint32_t mir_api_x100;		// (uint32_t)(_MIR_get_api_version() * 100)
};

// --- AOT ledger (forest-carriers S5) -------------------------------------
// The C-lane madc runtime, precompiled to MIR modules at pack time and
// carried in THIS container so a `-static-libmadc` emit can merge the pieces
// a program needs into its own image (no libmadc.so.0 / dylib at run time).
// Rides every carrier the forest rides (self-image, library image, sidecar,
// $MADC_FOREST) because it IS a forest segment.
//
// Read INDEPENDENTLY of the grove bind: the ledger is madc's own runtime,
// so it is target-specific but dialect-agnostic — the v27 producer-config
// gate (language std + -D fold) must NOT keep a --std=c99 compile from
// linking the runtime. Only the footer / context-hash / version pins apply.
//
// OPTIONAL, like the MIR module cache: a container without the segment is
// well-formed and simply carries no ledger (-static-libmadc then refuses
// loudly). That is why adding it needs no format-version bump.
struct cir_forest_ledger_header
{
	uint32_t forest_version;	// CIR_FOREST_FORMAT_VERSION at pack
	uint32_t mir_api_x100;		// (uint32_t)(_MIR_get_api_version() * 100)
	uint32_t module_count;
	uint32_t _pad;
};

// Per-module directory entry, immediately following the header (one per
// module, in canonical order); the name / symbol / byte blocks follow the
// whole directory, each addressed by these lengths in the same order.
// (The DECODED form, cir_ledger_module, lives in madc_cir.h — the emit lane
// consumes it too, and this header holds only the wire shapes.)
struct cir_forest_ledger_entry
{
	uint32_t name_bytes;	// module name (the ledger source path)
	uint32_t sym_bytes;	// NUL-separated defined-symbol names
	uint32_t mir_bytes;	// MIR_write_module bytes
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
	CIR_GLOBALF_EXTERN_REF       = 1u << 3,	// v22: `extern T name;` REFERENCE to a library-defined
						// object (std::cout) — no storage, no ctor; the flush
						// rebuilds live's vfEXTERN Variable + Itanium
						// storage_alias_name (namespace_cpp_variable_symbol)
	CIR_GLOBALF_TU_ROOT          = 1u << 4,	// v24: declared in the TU's ROOT file (the program) —
						// fenced from the bind restore (forest = #include
						// state only)
	CIR_GLOBALF_CTOR_ARG_TOKENS  = 1u << 5	// v25: ctor-syntax initializer `T name(args);` (the
						// <compare> ordering constants' out-of-class static
						// member definitions) — the args list's RAW SOURCE
						// TOKEN run rides ctor_tok_* (arena tokbytes); the
						// flush re-runs the live args-list parse over it to
						// rebuild TokenDecl::ctor_args, so global_ctor_call
						// selects the real ctor overload with the real args
						// (default-construction has no matching ctor here)
	,
	CIR_GLOBALF_CONST_SCALAR     = 1u << 6	// v26: a parse-time CONSTANT scalar with NO TopDecl —
						// a plain / anonymous enum's ENUMERATOR (TokenENUM's
						// global branch: addVariable + set + makeconstant;
						// references FOLD at parse, no storage, live emits
						// none). init_value holds the folded value; the
						// flush rebuilds the live registration exactly.
						// Provenance rides Program::forest_enum_const_origin
						// (the funcdef_files precedent).
	,
	CIR_GLOBALF_SCALAR_UNINIT    = 1u << 7	// an UNINITIALIZED file-scope definition (`int REQ;`,
						// a plain-C tentative definition — also plain
						// struct/array globals): live emits a bss item via
						// var_decl with NO initializer, so the flush leaves
						// the rebuilt TokenDecl's initialize NULL. No
						// init_value. (smaug_requests_source's REQ.)
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
	// v25 (CIR_GLOBALF_CTOR_ARG_TOKENS): the ctor-args token run, in the arena
	// tokbytes block (same span shape as paramrec's default run; file_id in the
	// arena strings pool). A layout change — covered by the v25 version bump.
	uint32_t ctor_tok_off;
	uint32_t ctor_tok_bytes;
	uint32_t ctor_tok_count;
	uint32_t ctor_file_id;
	// v39: Variable::storage_alias_name VERBATIM (intern handle; 0 = none) —
	// the emitted symbol this reference resolves to. The producer computed it
	// at parse through whichever derivation OWNS the entity's category, so it
	// is transported, never re-derived: the consumer had one derivation
	// (namespace_cpp_variable_symbol) and used it for every record, which
	// mangled a class-scope STATIC DATA MEMBER's flat key `Tag__member` as a
	// single identifier component (_ZSt14ctype_char__id — a name no library
	// exports) instead of the nested _ZNSt3__15ctypeIcE2idE. LOADED == parsed
	// applies to derived names too.
	uint32_t alias_id;
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
	CIR_TMPLF_OOL_MEMBER_TMPL     = 1u << 3,	// OutOfLineMemberDef::is_member_template
	CIR_TMPLF_TU_ROOT             = 1u << 4		// v24: pattern captured in the TU's ROOT file —
							// fenced from the bind restore
};
enum : uint32_t {	// cir_forest_template_param::pflags
	CIR_TMPLP_IS_TYPE = 1u << 0,
	CIR_TMPLP_IS_PACK = 1u << 1,
	CIR_TMPLP_IS_INNER = 1u << 2	// OUTOFLINE: a MEMBER-template (inner) param
};
enum : uint32_t {
	CIR_CLASS_PATTERN_MAGIC = 0x43504154u,	// "CPAT"
	// v4 adds direct-template-parameter spelling provenance to method params;
	// stale payloads cannot reproduce bound typedef identity exactly.
	CIR_CLASS_PATTERN_PAYLOAD_VERSION = 4
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
	uint32_t pattern_begin;	// u32 word offset of the ClassPattern payload slice
	uint32_t pattern_words;	// zero when capture failed or the definition is bodyless
	uint32_t pattern_reason;	// Program::ClassParseReason wire value
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
	madc::dis::decode_vector<uint64_t>       connectors;	// (target_unit << 32) | target_record
	madc::dis::decode_vector<cir_frozen_pos> positions;	// parallel to blob.records
	// --- grove payload v2 (B4a) ---
	std::vector<uint8_t>        token_payload;	// .madh record form
	uint32_t                    token_count = 0;
	std::vector<cir_forest_decl_entry> decl_index;
	std::vector<uint32_t>       pp_events;	// cir_forest_pp_event stream
	std::vector<uint32_t>       edges;	// directory unit indices, include order
	// v40: external branch dependencies — [name_id, flags, value_id,
	// definer_unit] tuples (flags bit0 = defined at freeze, bit1 =
	// value_id valid; definer_unit 0xffffffff = none).
	std::vector<uint32_t>       branch_deps;
	std::vector<uint8_t>        source_payload;	// v41: pre-PP source bytes
};

// A whole frozen forest in memory (the multi-unit sibling of cir_frozen_blob).
struct cir_frozen_forest
{
	std::vector<cir_forest_unit> units;
	uint32_t                     root_unit;
	uint32_t                     root_idx;
	// v27: the producer's compile config (madc_forest_config_word /
	// madc_forest_defines_hash) — bind requires an exact consumer match.
	uint32_t                     language_std = 0;
	uint32_t                     defines_hash = 0;
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
	// tokens carry the template's header origin. v25: this is the BODY-origin
	// provenance the body-stamping rule fences by (root-vs-include — the v24
	// discriminator); the defrec's DF_TU_ROOT_ORIGIN carries the DECLARATION
	// provenance (decl_file), which differs when a header prototype meets a
	// root-file definition. Falls back to the unit name when absent.
	std::map<std::string, const char *> funcdef_files;
};

// The context-hash pin: madc version + record/position layout + the c2mir
// node-code enum tail + the typeid primitive tail. Stamped by writers,
// REQUIRED equal by CirFrozenForest::open (reject-and-fail, never mis-thaw).
uint64_t madc_cir_context_hash();

// v27 producer-config gate (ONE derivation shared by freeze and bind —
// defined in cir_freeze.cpp next to the context hash):
// the effective language standard word (LanguageStd | gnu_dialect<<16) and a
// fold of the -D cli defines. ensure_bind_forest requires both equal to the
// container's, else it falls through to live parse (a per-compile check, so
// it cannot ride the process-invariant context hash).
class Program;
uint32_t madc_forest_config_word(const Program *prog);
uint32_t madc_forest_defines_hash(const Program *prog);

// Partition the sub-DAG rooted at `root` into per-unit segments keyed by
// each node's origin-token source file (origin-less nodes inherit their
// discovering parent's unit; the root falls back to `main_unit_name`).
// Optional unit-ownership override for the record partition: given a node's
// origin token slot-id, return the unit name that OWNS the node, or NULL for
// the default (the origin token's source file). The bridge layer uses this
// for __need protocol servings — a serving's nodes belong to the INCLUDER's
// unit, so no husk unit forms under the served header's name (this layer
// stays Program-blind; the resolver lives with the caller).
typedef const char *(*cir_unit_owner_fn)(void *ctx, uint32_t origin_id);

// Interns unit names, string payloads, and position file names into the
// ACTIVE string pool — serialize that pool into the same container after
// this call. False on a null root or an out-of-format tree.
bool cir_freeze_forest(cir_node *root, const char *main_unit_name,
		       cir_frozen_forest &out,
		       cir_unit_owner_fn owner_override = NULL,
		       void *owner_ctx = NULL);

// Stage a complete forest into a container: directory, string-pool blocks
// (the active pool, whose ids all forest handles reference), typeid->name
// closure, and every unit's four payload segments. The caller still sets
// the context hash (cir_forest_write does it) and picks placement
// (write_file / append_file / build). `zstd_level` (Zstd codec only):
// the RELEASE pack passes high (paid once per release build, budgeted
// against the per-process CPU cap — see the madc_cir.cpp call site);
// dev/standalone freezes keep the fast default so the drain-ladder loop
// stays cheap. `compress_intern` compresses the three intern-spine blocks
// too (release pack only — consumers rebind via the owned-buffer fallback
// at ~7ms once per process instead of zero-copy; owner-approved trade,
// task #37).
bool cir_forest_write(const cir_frozen_forest &f, madc::dis::snapshot_writer &w,
		      PchCompression codec = PchCompression::Zlib,
		      int zstd_level = 0, bool compress_intern = false);

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
	madc::dis::decode_bytes _record_planes;	// forest byte-plane records, kept columnar
	size_t _record_count;
	madc::dis::decode_vector<uint64_t> _connectors;	// forest units only
	madc::dis::decode_vector<cir_frozen_pos> _positions;	// forest units only
	CirFrozenForest *_forest;		// NULL = standalone (B2 mode)
	c2m_ctx_t _c2m;
	uint32_t _seg;				// registered segment id
	std::vector<cir_node *> _mat;		// per-record memo (NULL = cold)
	std::deque<cir_node> _nodes;		// materialized node storage (stable)

	bool record_at(uint32_t idx, cir_frozen_record &out) const;
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
	CirFrozenSegment(cir_forest_unit &&unit,
			 madc::dis::decode_bytes &&record_planes,
			 size_t record_count, CirFrozenForest *forest,
			 c2m_ctx_t c2m);
	~CirFrozenSegment();

	uint32_t seg() const { return _seg; }
	size_t record_count() const { return _record_count; }
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
	bool        flat_alias;		// v26 (CIR_TYPEK_TYPEDEF): the producer's FLAT
					// datatype_map also held this key -> same
					// definition (the explicit-specialization
					// alias_key surface) — the restore reproduces
					// the flat datatype_map + struct_map writes
	bool        tag_alias;		// CIR_TYPEK_TYPEDEF: the producer's struct_map
					// also held this key -> same definition (a
					// struct/class-KEYWORD typedef registers the
					// alias as a tag) — the restore reproduces the
					// struct_map write so `struct X` resolves
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
	const char *alias;		// v39: storage_alias_name verbatim (NULL = none)
	int64_t     init_value;		// scalar integer init (valid iff CIR_GLOBALF_SCALAR_INIT) — v14
	// v25 (CIR_GLOBALF_CTOR_ARG_TOKENS): the ctor-args raw-token run — a span
	// into the arena tokbytes; the parser-side flush deserializes it and
	// re-runs the live args-list parse to rebuild TokenDecl::ctor_args.
	const uint8_t *ctor_bytes;	// NULL = no run
	uint32_t       ctor_len;
	uint32_t       ctor_count;
	const char    *ctor_file;	// origin file (NULL = none)
};

// v25: restored namespace-surface state. An NSLINK is an inline-namespace
// parent -> child link (the flush re-runs mirror_inline_namespace_into_parent
// over restored state); an NSBIND is a using-declaration function import
// (namespace_map[ns][name] -> the fn registered under funcdef key).
struct CirRestoredNsLink
{
	const char *parent;		// NULL/"" = global
	const char *child;		// the inline namespace's full name
};
struct CirRestoredNsBind
{
	const char *ns;			// the importing namespace
	const char *name;		// the visible name
	const char *key;		// the imported fn's funcdef_map key
	// True = the import is a MEMBER of ns::name's overload set on the
	// live side ([namespace.udecl] join; DF_NSBIND_OVERLOAD_MEMBER) —
	// the flush joins it back so a bound consumer ranks the SAME set
	// the freezing parse ranked. Plain map rebinds (inline-ns mirror
	// redundancy) stay bind-only: the live mirror grows no sets.
	bool ov_member;
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
	// v26 piece (a): the fn's NAMED parameters (the aliasrec run on its
	// DK_FUNC record) — the flush fills Method::parameters from these so a
	// deferred body's re-parse resolves its parameter names (the scope
	// parse_deferred_function_body's code->method provides).
	std::vector<std::pair<const char *, DataDef *> > mparams;
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
	const uint32_t *pattern;		// ClassPattern payload words (NULL = absent)
	uint32_t pattern_words;
	uint32_t pattern_reason;
	// slice B1 (task #25): raw record offsets for LAZY payload hydration.
	// The materialize walk fills IDENTITY only (the fields above through
	// pattern_reason, minus params/runs/pattern); everything payload-backed
	// decodes in hydrate_restored_template() on first demand, so a TU pays
	// for the templates it instantiates, not the ones its headers declare.
	uint32_t rec_param_begin = 0, rec_param_count = 0;
	uint32_t rec_run_begin = 0, rec_spec_count = 0;
	uint32_t rec_pattern_begin = 0, rec_pattern_words = 0;
	bool hydrated = false;		// hydrate ran (either way)
	bool hydrate_failed = false;	// payload bounds broken — drop the def
};

// v26: one restored deferred-method-body entry (DK_DEFBODY). The parser-side
// flush deserializes the four runs and rebuilds Program::deferred_lazy_bodies
// [key] with var = the restored method Variable, so the existing m&l fixpoint
// materializes the body on first ODR-use exactly as live.
struct CirRestoredDeferredBody
{
	const char *key;		// deferred_lazy_bodies key (== the Variable's name)
	DataDefCLASS *owner;		// swizzled owner class (NULL = none restored)
	const char *file;		// origin file (NULL = none)
	int line, column;
	bool full_definition;
	CirRestoredTemplateRun runs[4];	// body / definition / trailing_ret / ctor_init
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
// Rung 2a (closure-filtered materialization): the demand filter the parser
// hands to materialize_for (forest_restore_decls). It carries the SAME
// verdict map item 5's registration filter builds (forest_restore_decls):
// decl-index name -> "some declaring unit is in the TU's bound-include
// closure". materialize_from_arena consults it with the SAME fallback chain
// as registration (qualified ns::name, then bare name, then unindexed =
// derived entity = keep; a `<`-bearing record judged by its canonical
// spelling HEAD). Skipped records never allocate DataDefs; an admitted
// record's references PULL skipped aggregates/enums back in, so bound
// chains never break. Inactive (the default) = whole-container
// materialization — direct restores, unit tests, and --run-frozen are
// untouched.
struct CirMaterializeFilter
{
	bool active = false;
	std::unordered_map<std::string, bool> declared_bound;
};

// --show-stats (task #25 slice D): ONE depth-guarded forest-work clock, the
// InstTimer discipline (parser.cpp). Every forest entry point — probe/open,
// bind walk, decl restore, unit-segment load, arena materialize, template
// payload, extern-index build — opens a frame on the owning Program's clock;
// only the OUTERMOST frame accumulates wall time, so nested entries are never
// double counted. The stats display samples the clock at the tokenize/parse/
// cir-build boundaries so those phase buckets report NET compute with the
// forest share carved into its own lines. Null clock (standalone consumers:
// unit harness, --run-frozen tools) = every frame no-ops.
struct ForestWorkFrame
{
	double *secs;
	int *depth;
	bool outer;
	std::chrono::steady_clock::time_point t0;
	ForestWorkFrame(double *s, int *d) : secs(s), depth(d), outer(false)
	{
		if (!secs || !depth)
			return;
		outer = ((*depth)++ == 0);
		if (outer)
			t0 = std::chrono::steady_clock::now();
	}
	~ForestWorkFrame()
	{
		if (!secs || !depth)
			return;
		if (outer)
			*secs += std::chrono::duration<double>(
				std::chrono::steady_clock::now() - t0).count();
		--(*depth);
	}
};

class CirFrozenForest
{
	friend class CirFrozenSegment;

	madc::dis::snapshot_reader _reader;
	c2m_ctx_t _c2m;
	std::vector<cir_forest_dir_unit> _units;
	std::vector<CirFrozenSegment *> _segs;	// lazily constructed per unit
	std::vector<std::string> _libs;
	std::vector<uint32_t> _lib_ids;	// dir lib name-ids (resolved in complete_open)
	std::map<uint32_t, uint32_t> _live_ids;	// frozen str id -> live pool id
	std::map<std::string, uint32_t> _unit_by_name;	// unit-name spelling -> index (Phase 6 bind)
	uint32_t _root_unit, _root_idx;
	uint32_t _language_std = 0, _defines_hash = 0;	// v27 producer config (dir header)

	// The container's own string pool (A1 frozen view over the three
	// blocks; decompressed copies owned here when the segments are
	// compressed, bound in place when codec is None).
	madc::dis::frozen_intern_table _pool;
	madc::dis::decode_bytes _pool_bytes, _pool_entries, _pool_buckets;
	// v2 container-global payloads (loaded at open; empty on v2-less
	// module containers).
	std::vector<uint32_t> _branch_macros, _canon_order;
	// The DataDef objects materialize lazily at bind (materialize_from_arena),
	// swizzling arena id refs back to pointers. The forest owns the
	// materialized objects and frees them in ~CirFrozenForest.
	std::vector<DataDef *> _mat_storage;		// forest-owned materialized DataDefs
	// Producer tid -> restored DataDef (materialize_from_arena's swizzle
	// map, kept for post-restore consumers: lazy ClassPattern payload
	// reads swizzle their concrete_type_id through it).
	std::map<uint32_t, DataDef *> _defs_by_tid;
	std::vector<Variable *> _mat_vars;		// forest-owned reconstructed method Variables
	std::vector<CirRestoredType> _restored;		// bind-facing view (built once)
	// v13 container-global: file-scope global var defs. Records load at open();
	// _restored_globals (type-ids swizzled to DataDef*) is built by
	// materialize_from_arena.
	std::vector<cir_forest_global_record> _globals;
	std::vector<CirRestoredGlobal> _restored_globals;
	std::vector<CirRestoredNsLink> _restored_nslinks;	// v25: inline-ns links
	std::vector<CirRestoredNsBind> _restored_nsbinds;	// v25: using-decl fn imports
	std::vector<CirRestoredDeferredBody> _restored_defbodies;	// v26
	// RC2: restored free-function declarations, built by materialize_from_arena
	// from the DF_IS_FREE_FUNC DK_FUNC records.
	std::vector<CirRestoredFunc> _restored_funcs;
	// v20 container-global: template-NAME state. Records + payload + token
	// bytes load at open(); _restored_templates (names resolved, owner
	// swizzled, runs exposed as spans) is built by materialize_from_arena.
	std::vector<cir_forest_template_record> _templates;
	// R1: the template payload + token-byte segments (the heavy pair —
	// ~5 MB raw on the packed corpus) decode LAZILY via
	// ensure_template_payload(): at the first filter-surviving template
	// record in materialize, or at a late ClassPattern run read. A TU
	// whose bound closure declares no templates (trivial C) never pays.
	// SPANS, not owned copies: the bytes live in the mapped image or the
	// process-level decoded-segment cache (both process-lifetime), so N
	// forests in one process share ONE decode and zero per-forest copies
	// (an 11-TU launch paid the multi-MB decode + copy per TU).
	// Mutable: restored_template_run is a const reader.
	mutable const uint32_t *_template_payload = NULL;
	mutable size_t _template_payload_words = 0;
	mutable const uint8_t *_template_tokens = NULL;
	mutable size_t _template_tokens_len = 0;
	mutable bool _template_payload_loaded = false;
	bool ensure_template_payload() const;
	std::vector<CirRestoredTemplate> _restored_templates;
	// v20 container-global: extern-decl index (symbol -> frozen location).
	// R1 (startup latency): built LAZILY on the first extern_loc_for query
	// — a compile that never asks (trivial C) never decodes the segment.
	std::map<std::string, std::pair<uint32_t, uint32_t> > _extern_by_name;
	bool _extern_index_built = false;
	// R1: two-stage open. open_header = footer + pin + directory (cheap);
	// complete_open = pools/arena/global segments (heavy, memoized).
	bool _header_opened = false;
	bool _fully_opened = false;
	// v18 container-global: the B3 DefArena dump (segments 10-14) — THE type-graph
	// serialization — bound read-only at open: in place when a segment is
	// uncompressed, else into the owned buffers. A type-less freeze binds an
	// empty view (zero-length segments).
	madc::dis::FrozenDefArena _arena;
	madc::dis::decode_bytes _arena_defs, _arena_payload;
	madc::dis::decode_bytes _arena_sbytes, _arena_sentries, _arena_sbuckets;
	madc::dis::decode_bytes _arena_tokbytes;	// v23: param-default token runs
	// v23: per-FuncDef param-default token runs (paramrec.def_tok_*), built by
	// materialize_from_arena alongside the methods/free functions. The parser's
	// pending-funcs flush deserializes each run and re-runs parseExpression.
	std::vector<CirRestoredFuncDefaults> _restored_param_defaults;
	bool _types_materialized;
	// Rung 2a: the closure demand filter (inactive by default — whole
	// container). materialize_for installs it on the first generation and
	// UNIONS later, wider filters in (S2 incremental materialization).
	CirMaterializeFilter _mat_filter;
	// S2 (R4-lite): what earlier generations already EMITTED, so a later,
	// wider filter re-runs the passes without duplicating. _mat_done_slots
	// covers the arena-slot-keyed walks (typedef / ns-surface / enum-record
	// / free-func — one record kind per slot, marked only on PUSH, so a
	// slot a narrower filter skipped is re-judged); globals/templates are
	// indexes into their record vectors. _method_by_func_id persists method
	// DEFINERS so a later generation's using-decl import resolves a definer
	// built in an earlier one. Aggregate/enum dedup needs no set: pass 1/1a
	// skip tids already in _defs_by_tid, and pass 2 fills only this
	// generation's fresh allocations.
	std::set<uint32_t> _mat_done_slots;
	std::set<size_t> _mat_done_globals;
	std::set<size_t> _mat_done_templates;
	std::map<uint32_t, Variable *> _method_by_func_id;
	// The pass engine both entry points share: runs every materialization
	// pass under the CURRENT _mat_filter, skipping records earlier
	// generations built (the guards above).
	void materialize_pass();
	// Shared v2 segment reader: decompress unit slot `slot` into `out`
	// (raw bytes). False on absent/malformed.
	bool read_unit_seg(uint32_t unit, uint32_t slot, uint32_t kind,
			   madc::dis::decode_bytes &out) const;

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
	// quiet_missing: a missing/absent container returns false SILENTLY —
	// the default-on bind probes /proc/self/exe on every compile and a
	// blob-less binary is the normal live-parse case, not an error
	// (real corruption and pin mismatches stay loud).
	bool open(const void *image, size_t len, c2m_ctx_t c2m,
		  bool quiet_missing = false);

	// R1 (startup latency): the cheap header stage of open() — footer,
	// context-hash pin, and the directory (unit table, root, v27
	// producer-config words). Enough for the bind path to config-gate
	// BEFORE paying the pool/arena binds; a mismatch then costs ~nothing.
	// complete_open() finishes the job (memoized; needs the live string
	// pool). open() remains the one-call composition of both.
	bool open_header(const void *image, size_t len,
			 bool quiet_missing = false);
	bool complete_open(c2m_ctx_t c2m);
	bool previous_image_len(size_t &len) const
		{ return _reader.previous_image_len(len); }

	// Rebind the c2m used to materialize nodes. The parse-time bind forest is
	// opened with c2m=NULL (it restores only types/PP, never node segments), so
	// the session c2m is set here before the first node materialization (inline
	// method body load at emit time). Safe iff no segment has materialized yet.
	void set_c2m(c2m_ctx_t c2m) { _c2m = c2m; }

	// Rung 2a + S2 (R4-lite): ensure everything `want` admits is
	// materialized — THE restore entry (forest_restore_decls). The first
	// caller installs `want` and materializes under it; a later, WIDER
	// filter (a bound verdict flipping false -> true, or want.active ==
	// false = whole container) unions in and re-runs the passes, which
	// skip already-built records. A narrower or equal filter is a no-op —
	// monotone widening, so shared-forest consumers can never lose records
	// to another TU's earlier, narrower view.
	const std::vector<CirRestoredType> &materialize_for(
		const CirMaterializeFilter &want);

	uint32_t unit_count() const { return (uint32_t)_units.size(); }
	size_t units_loaded() const;			// laziness observability

	// --show-stats observability (startup-latency R0): where forest wall
	// time goes, split by owner. The reader's decode counters (zstd frames
	// / bytes / secs, codec-None copies) are forwarded so consumers never
	// need the reader itself.
	double _stat_open_secs = 0.0;	// open(): dir + pools + arena bind + name indexes
	double _stat_unitload_secs = 0.0;	// unit_segment(): node-record decode + validate
	unsigned long long _stat_unitload_count = 0;
	double _stat_mat_secs = 0.0;	// materialize_from_arena(): DataDef rebuild
	// task #25 slice D: the owning Program's depth-guarded forest-work clock
	// (Program::_forest_work_seconds / _forest_work_depth), installed by
	// ensure_bind_forest so forest-side entries share ONE clock with the
	// Program-side ones. Null in standalone consumers — frames no-op.
	double *_work_secs = 0;
	int    *_work_depth = 0;
	unsigned long long stat_zstd_frames() const { return _reader.stat_zstd_frames; }
	unsigned long long stat_zstd_bytes_out() const { return _reader.stat_zstd_bytes_out; }
	double             stat_zstd_secs() const { return _reader.stat_zstd_secs; }
	unsigned long long stat_copy_calls() const { return _reader.stat_copy_calls; }
	unsigned long long stat_copy_bytes() const { return _reader.stat_copy_bytes; }
	// v27 producer config (bind gate: both must equal the consumer's).
	uint32_t language_std() const { return _language_std; }
	uint32_t defines_hash() const { return _defines_hash; }
	const std::vector<std::string> &libs() const { return _libs; }
	const char *unit_name(uint32_t unit) const;
	// Reverse directory (Phase 6 bind): unit-name spelling -> unit index, or
	// -1 if no unit carries that name. Built once in open() over the directory.
	// The key is the exact unit_name string — a resolved include path or a bare
	// compiler-builtin/embedded name (e.g. "stddef.h").
	int find_unit(const std::string &name) const;
	// Machine-portable fallback: the unit whose path-name ends in
	// "/<incfile>" (see the .cpp comment — consumers on machines without
	// the producer's header tree cannot spell full-path unit names).
	int find_unit_path_tail(const std::string &incfile) const;
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
	bool unit_branch_deps(uint32_t unit, std::vector<uint32_t> &out);	// v40
	bool unit_has_source(uint32_t unit) const;	// v41 directory/segment probe
	bool unit_source(uint32_t unit, std::string &out) const;	// v41 raw source fallback
	const std::vector<uint32_t> &branch_macros() const { return _branch_macros; }
	const std::vector<uint32_t> &canon_order() const { return _canon_order; }
	// MIR module cache: the container's compiled-module blob (raw
	// MIR_write_module bytes, stamp header validated and stripped). False =
	// no cache segment (the normal blob-less case), stamp mismatch (logged
	// loud — a mismatched cache means a coupling bug, not routine fallback),
	// or a malformed segment. Reader-only; independent of unit/node state.
	bool mir_module_bytes(std::vector<uint8_t> &out) const;
	// AOT ledger (S5): the C-lane runtime modules this container carries,
	// with their defined-symbol index. False = no ledger segment (the
	// normal case for a container packed without --freeze-ledger=), stamp
	// mismatch, or a malformed segment (both logged loud). Reader-only and
	// deliberately NOT gated on the producer-config match — the ledger is
	// madc's own runtime, not dialect-dependent parse state.
	bool ledger_modules(std::vector<cir_ledger_module> &out) const;
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
	// Producer tid -> restored DataDef. Valid after materialize_from_arena()
	// (pinned primitives resolve regardless). Lazy ClassPattern payload
	// reads use this to swizzle serialized concrete_type_id values.
	DataDef *restored_def_by_tid(uint32_t tid) const;
	// v13: the restored file-scope globals (type-ids swizzled to DataDef*). Valid
	// after materialize_from_arena() — call it first (it builds this view
	// alongside the types, reusing the same arena-id -> DataDef* map).
	const std::vector<CirRestoredGlobal> &restored_globals() const
	{ return _restored_globals; }
	const std::vector<CirRestoredNsLink> &restored_nslinks() const
	{ return _restored_nslinks; }		// v25
	const std::vector<CirRestoredNsBind> &restored_nsbinds() const
	{ return _restored_nsbinds; }		// v25
	const std::vector<CirRestoredDeferredBody> &restored_defbodies() const
	{ return _restored_defbodies; }		// v26
	// RC2: the restored free-function declarations. Valid after
	// materialize_from_arena() — call it first.
	const std::vector<CirRestoredFunc> &restored_funcs() const
	{ return _restored_funcs; }
	// v20: the restored template-NAME state (metadata + token-run spans).
	// Valid after materialize_from_arena() — call it first. slice B1: the
	// walk restores IDENTITY only; a consumer that needs params/runs/
	// pattern hydrates the record first (non-const overload + hydrate).
	const std::vector<CirRestoredTemplate> &restored_templates() const
	{ return _restored_templates; }
	std::vector<CirRestoredTemplate> &restored_templates()
	{ return _restored_templates; }
	// slice B1 (lazy template payloads): decode ONE record's params +
	// token-run table + ClassPattern payload slice, memoized per record.
	// False = payload bounds broken (corrupt container) — the caller drops
	// the definition, exactly as the eager walk's `continue` did.
	bool hydrate_restored_template(CirRestoredTemplate &rt) const;
	const char *restored_template_string(uint32_t id) const;
	CirRestoredTemplateRun restored_template_run(
		const cir_forest_token_run &run) const;
	// v23: per-FuncDef param-default token runs. Valid after
	// materialize_from_arena() — call it first.
	const std::vector<CirRestoredFuncDefaults> &restored_param_defaults() const
	{ return _restored_param_defaults; }
	// v20: the frozen location of the producer's top-level extern decl for
	// `sym` (false = none indexed). The bind layer loads the decl node via
	// node_for when a loaded body references the symbol. The index builds
	// on the first query (R1) — see _extern_by_name.
	bool extern_loc_for(const std::string &sym,
			    uint32_t &unit, uint32_t &idx);
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
