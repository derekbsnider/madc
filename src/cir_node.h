/* cir_node.h — Extended c2mir node with madc-specific fields.
 *
 * cir_node extends c2mir's struct node (at offset 0) so that a
 * cir_node* IS-A node_t* and can be passed directly to c2mir's
 * checker and MIR generator.  The extension fields carry madc AST
 * info, typedef names, source positions, and language tags — enough
 * to reconstruct the original source from the tree.
 *
 * Memory: madc owns all cir_node objects via CirArena.  c2mir never
 * allocates or frees them.  c2mir's checker writes attr pointers
 * (via reg_malloc) into our nodes; those are freed by c2mir_finish.
 *
 * Serializable references (forest B1): every madc EXTENSION field is
 * position-independent — a typeid, an intern-table/value-pool handle, or a
 * (segment, node-index) cir_ref — so the extension block of a node record
 * freezes as-is.  Only the c2mir-VISIBLE `base` keeps raw pointers live
 * (op links, u.s string payloads, attr): the live tree stays pointer-based
 * node_t by decision (forest plan SETTLED #1/#7); those base fields are
 * mapped to refs/handles at freeze time (B2) and swizzled back to real
 * pointers at the c2mir edge on load.
 */

#ifndef __CIR_NODE_H
#define __CIR_NODE_H 1

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "c2mir/c2mir_node.h"
#include "c2mir/c2mir_api.h"
}

// Forward declarations
class TokenBase;
class DataDef;
class CirArena;
struct cir_node;

// Source language tag — allows regenerating code in the original language
enum CirSourceLang : uint8_t {
	cslC       = 0,  // C11/C23
	cslCxx     = 1,  // C++23
	cslMadc    = 2,  // madc scripting language
	cslOther   = 3   // python, ruby, rust, php — future
};

// ---------------------------------------------------------------------------
// cir_ref: serializable cross-node reference
// ---------------------------------------------------------------------------
// (segment-id, node-index) — the forest's position-independent node address
// (execution plan SETTLED #5): two uint32s behind ONE inline resolve accessor
// (madc_cir_node_for below), NOT hand-packed bits.  seg 0 is reserved as the
// null reference, so a zeroed record is a null ref by construction.

struct cir_ref {
	uint32_t seg;   // segment id (0 = null reference; live arenas start at 1)
	uint32_t idx;   // node index within the segment

	bool valid() const { return seg != 0; }
	bool operator==(const cir_ref &o) const { return seg == o.seg && idx == o.idx; }
};

// Segment registry: every CirArena self-registers at construction and gets a
// segment id; frozen forest segments join the same id space when they land
// (B2/B3).  madc_cir_node_for is THE resolve(ref) chokepoint — all ref
// dereferencing goes through it (the call_emit_symbol discipline).
uint32_t  madc_cir_register_segment(CirArena *arena);
void      madc_cir_unregister_segment(uint32_t seg);
cir_node *madc_cir_node_for(cir_ref ref);   // NULL for a null/foreign ref

// ---------------------------------------------------------------------------
// cir_node: struct node + madc extensions
// ---------------------------------------------------------------------------
// IMPORTANT: struct node MUST be the first member so that
//   (node_t)(&cir_node_instance) == &cir_node_instance.base
// and the DLIST macros (which access op_link at a fixed offset) work.

struct cir_node {
	struct node base;           // offset 0 — c2mir-compatible layout

	// --- madc extension fields (invisible to c2mir) ---
	// ALL of these are serializable by construction: indices and handles,
	// never raw pointers.  Reads/writes go through the accessors below,
	// which resolve against the active substrate (type table, string pool,
	// segment registry) — the same static-hook discipline as
	// TokenBase::_active_strpool / _active_valpool.
	//
	// `origin_id` is the SINGLE SOURCE OF TRUTH for this node's source position
	// and provenance: a stable TokenArena slot-id (0 = synthetic / no origin).
	// The source position is a DERIVED VIEW (src_file()/src_line()/src_column()
	// below, which resolve the id via madc_token_for_slot) — never stored as
	// independent absolute truth on the node.
	uint32_t     origin_id;     // originating madc token's arena slot-id (0 = synthetic)
	uint32_t     datadef_id;    // madc type info as a typeid (madc_typeid.h
				    // segments; 0 = not type-related).  Resolve via
				    // datadef()/set_datadef() — exact DataDef*
				    // round-trip through the type table.
	uint32_t     typedef_name_id; // source typedef alias as a strpool handle
				    // (e.g. "EXT_BV"); 0 = none
	uint32_t     error_msg_id;  // strpool handle; nonzero: error/incomplete
				    // node — a tree that contains one MUST NOT be
				    // handed to c2mir
	CirSourceLang src_lang;     // which language produced this node
	bool         synth_from_origin; // true: a lowering artifact (e.g. a ctor/dtor
				    // call synthesized for a C++ construct). Shares its
				    // origin with the high-level node; the reverse-
				    // renderer emits the origin ONCE and suppresses these.

	// Two-tree tsubst marker: this node is the pattern operand of a C++ pack
	// expansion (`expr...`) in a saved Tree-1 body. c2mir ignores these fields;
	// CirBuilder::copy_cir_subtree consumes them by repeating this subtree into
	// the parent list using the concrete FuncDef::tsubst_type_arg_packs entry.
	bool         tsubst_pack_expand;
	uint32_t     tsubst_pack_index;
	uint32_t     tsubst_pack_value_id;  // pack value spelling, strpool handle

	// This node's own (segment, index) identity, stamped by CirArena::alloc()
	// (and by the segment loader when frozen segments land). What a copier
	// stores to reference this node position-independently.
	cir_ref      self;

	// Tree-1 back-reference: the immutable source node this node was COPIED
	// from (the tree-level analogue of `origin_id`, which links DOWN to a
	// token). Set by copy_cir_subtree; null for an originally-built node.
	// Invisible to c2mir (an extension field), so a mutable Tree-2 node may
	// safely reference its immutable Tree-1 source while c2mir compiles it.
	cir_ref      tree1_origin;

	// Cast to node_t for c2mir API calls
	node_t as_node() { return (node_t)&base; }

	// Accessors for the handle fields (defined in cir_builder.cpp where the
	// substrate hooks are complete).  The const char* results point into the
	// active string pool: valid until an intern that grows the pool — consume
	// immediately (copy/print/compare), hold the id, never the pointer.
	DataDef     *datadef() const;               // exact round-trip, NULL if none
	void         set_datadef(DataDef *dd);      // lazy-stamps dd->type_id
	const char  *typedef_name() const;          // NULL if none
	void         set_typedef_name(const char *s);
	const char  *error_msg() const;             // NULL if none
	void         set_error_msg(const char *s);
	const char  *tsubst_pack_value_name() const; // NULL if none
	void         set_tsubst_pack_value_name(const char *s);
	cir_node    *tree1_origin_node() const;     // resolve via madc_cir_node_for

	// Derived source position (read from the origin token; the single source
	// of truth). Defined in cir_builder.cpp where TokenBase is complete.
	const char *src_file() const;
	int         src_line() const;
	int         src_column() const;
};

// Recover cir_node* from a node_t.  Only valid if the node was
// allocated as a cir_node (i.e. by CirArena).
#define CIR_NODE(n) ((struct cir_node *)(n))

// Static assert: base must be at offset 0 for the IS-A cast to work.
// Use a C++11 static_assert.
static_assert(offsetof(cir_node, base) == 0,
	"cir_node::base must be at offset 0 for node_t compatibility");

// ---------------------------------------------------------------------------
// CirArena: pool allocator for cir_node objects
// ---------------------------------------------------------------------------
// Allocates cir_nodes in fixed-size pages.  Never frees individual nodes.
// Destroyed in bulk when the arena goes out of scope.  Registers itself as a
// cir segment so every node it hands out carries a resolvable (seg, idx)
// identity (see cir_ref above).

class CirArena {
	static const size_t PAGE_SIZE = 4096;  // nodes per page

	struct Page {
		cir_node nodes[PAGE_SIZE];
		size_t used;
		Page() : used(0) {}
	};

	std::vector<Page *> pages;
	uint32_t m_seg;                // this arena's segment id in the registry

public:
	CirArena()
	{
		pages.push_back(new Page());
		m_seg = madc_cir_register_segment(this);
	}

	~CirArena()
	{
		madc_cir_unregister_segment(m_seg);
		for (auto *p : pages) delete p;
	}

	uint32_t seg() const { return m_seg; }

	// Allocate a zeroed cir_node (all handle/ref fields start null) and
	// stamp its (seg, idx) identity.
	cir_node *alloc()
	{
		Page *p = pages.back();
		if (p->used >= PAGE_SIZE) {
			p = new Page();
			pages.push_back(p);
		}
		cir_node *cn = &p->nodes[p->used];
		memset(cn, 0, sizeof(cir_node));
		cn->self.seg = m_seg;
		cn->self.idx = (uint32_t)((pages.size() - 1) * PAGE_SIZE + p->used);
		p->used++;
		return cn;
	}

	// Node for a slot index handed out by alloc(); NULL past the live end.
	// The resolve(ref) chokepoint (madc_cir_node_for) dispatches here for
	// live-arena segments.
	cir_node *node_at(uint32_t idx)
	{
		size_t page = idx / PAGE_SIZE, slot = idx % PAGE_SIZE;
		if (page >= pages.size() || slot >= pages[page]->used)
			return NULL;
		return &pages[page]->nodes[slot];
	}
};

#endif // __CIR_NODE_H
