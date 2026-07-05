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

// Consumer-defined segment kinds for the content-blind snapshot container
// (a container may hold several logical payloads; the kind is the contract).
enum : uint32_t
{
	SNAP_KIND_CIR_RECORDS    = madc::dis::SNAP_KIND_CONSUMER + 0,	// cir_frozen_record[]
	SNAP_KIND_CIR_CHILDREN   = madc::dis::SNAP_KIND_CONSUMER + 1,	// uint32[] record indices
	SNAP_KIND_CIR_FOREST_DIR = madc::dis::SNAP_KIND_CONSUMER + 2,	// cir_forest_dir_header + units + libs
	SNAP_KIND_CIR_CONNECTORS = madc::dis::SNAP_KIND_CONSUMER + 3,	// uint64[] (unit<<32 | record)
	SNAP_KIND_CIR_POSITIONS  = madc::dis::SNAP_KIND_CONSUMER + 4,	// cir_frozen_pos[] parallel to records
	SNAP_KIND_CIR_TYPE_NAMES = madc::dis::SNAP_KIND_CONSUMER + 5,	// uint32 pairs {typeid, name_id}
	// --- grove payload v2 (B4a; design doc 2026-07-04 §2) ---
	SNAP_KIND_CIR_UNIT_TOKENS   = madc::dis::SNAP_KIND_CONSUMER + 6,  // post-PP token slice (.madh record form)
	SNAP_KIND_CIR_DECL_INDEX    = madc::dis::SNAP_KIND_CONSUMER + 7,  // cir_forest_decl_entry[]
	SNAP_KIND_CIR_PP_EXPORTS    = madc::dis::SNAP_KIND_CONSUMER + 8,  // uint32 event stream (cir_forest_pp_event + params)
	SNAP_KIND_CIR_UNIT_EDGES    = madc::dis::SNAP_KIND_CONSUMER + 9,  // uint32[] directory unit indices, include order
	SNAP_KIND_CIR_BRANCH_MACROS = madc::dis::SNAP_KIND_CONSUMER + 10, // container-global: uint32 name ids, sorted
	SNAP_KIND_CIR_CANON_ORDER   = madc::dis::SNAP_KIND_CONSUMER + 11  // container-global: uint32 unit indices, canonical order
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
enum : uint32_t { CIR_FOREST_FORMAT_VERSION = 2 };
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
	CIR_FOREST_SEG_TYPE_NAMES    = 5,
	CIR_FOREST_SEG_BRANCH_MACROS = 6,	// v2 (absent = zero-length)
	CIR_FOREST_SEG_CANON_ORDER   = 7,	// v2 (absent = zero-length)
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
	uint32_t _root_unit, _root_idx;

	// The container's own string pool (A1 frozen view over the three
	// blocks; decompressed copies owned here when the segments are
	// compressed, bound in place when codec is None).
	madc::dis::frozen_intern_table _pool;
	std::vector<uint8_t> _pool_bytes, _pool_entries, _pool_buckets;
	// v2 container-global payloads (loaded at open; empty on v2-less
	// module containers).
	std::vector<uint32_t> _branch_macros, _canon_order;
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

	uint32_t unit_count() const { return (uint32_t)_units.size(); }
	size_t units_loaded() const;			// laziness observability
	const std::vector<std::string> &libs() const { return _libs; }
	const char *unit_name(uint32_t unit) const;
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
