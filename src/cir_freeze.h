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
 * B2 scope fence (in-process mechanism + format): datadef_id (project
 * segment), origin_id (token slots), and string handles resolve against the
 * LIVE process substrate. Cross-process closure — binding the container's
 * own frozen intern/type segments, the context-hash pin — is B3 (the
 * multi-segment forest), per docs/plans/2026-06-22-embedded-header-forest-
 * execution-plan.md.
 */

#ifndef __CIR_FREEZE_H
#define __CIR_FREEZE_H 1

#include <cstdint>
#include <deque>
#include <vector>

#include "cir_node.h"
#include "madcdis/snapshot.h"

// Consumer-defined segment kinds for the content-blind snapshot container
// (a container may hold several logical payloads; the kind is the contract).
enum : uint32_t
{
	SNAP_KIND_CIR_RECORDS  = madc::dis::SNAP_KIND_CONSUMER + 0,	// cir_frozen_record[]
	SNAP_KIND_CIR_CHILDREN = madc::dis::SNAP_KIND_CONSUMER + 1	// uint32[] record indices
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

enum : uint32_t { CIR_FROZEN_CHILD_CONNECTOR_BIT = 0x80000000u };  // reserved (B3)

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

// A loaded frozen segment: joins the live (seg, idx) id space via the B1
// registry and materializes records to real cir_nodes on touch, at the
// c2mir edge (needs the c2m context for uids / uniq strings / positions).
// Owns both the records and the materialized node storage.
class CirFrozenSegment : public cir_segment_source
{
	cir_frozen_blob _blob;
	c2m_ctx_t _c2m;
	uint32_t _seg;				// registered segment id
	std::vector<cir_node *> _mat;		// per-record memo (NULL = cold)
	std::deque<cir_node> _nodes;		// materialized node storage (stable)

	cir_node *shell(uint32_t idx);		// phase A: node without children

public:
	CirFrozenSegment(cir_frozen_blob &&blob, c2m_ctx_t c2m);
	~CirFrozenSegment();

	uint32_t seg() const { return _seg; }
	size_t record_count() const { return _blob.records.size(); }
	size_t materialized_count() const;

	// THE resolve-on-touch entry: materialize the sub-DAG rooted at idx
	// (memoized; shares/cycles terminate) and return its real node.
	virtual cir_node *node_at(uint32_t idx);
};

// Structural identity oracle (forest Phase 2 gate): walk two trees in
// parallel — codes, payload class content, extension fields, child
// sequences — with the shared-subtree/cycle discipline of the dump walker.
// Returns false at the first divergence.
bool cir_trees_structurally_identical(node_t a, node_t b);

#endif // __CIR_FREEZE_H
