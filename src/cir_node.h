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

// Source language tag — allows regenerating code in the original language
enum CirSourceLang : uint8_t {
	cslC       = 0,  // C11/C23
	cslCxx     = 1,  // C++23
	cslMadc    = 2,  // madc scripting language
	cslOther   = 3   // python, ruby, rust, php — future
};

// ---------------------------------------------------------------------------
// cir_node: struct node + madc extensions
// ---------------------------------------------------------------------------
// IMPORTANT: struct node MUST be the first member so that
//   (node_t)(&cir_node_instance) == &cir_node_instance.base
// and the DLIST macros (which access op_link at a fixed offset) work.

struct cir_node {
	struct node base;           // offset 0 — c2mir-compatible layout

	// --- madc extension fields (invisible to c2mir) ---
	// `origin_id` is the SINGLE SOURCE OF TRUTH for this node's source position
	// and provenance: a stable TokenArena slot-id (0 = synthetic / no origin),
	// NOT a raw pointer — so the node link to the token layer is an INDEX,
	// serializable to disk (part of "all pointer classes become indices"). The
	// source position is a DERIVED VIEW (src_file()/src_line()/src_column()
	// below, which resolve the id via madc_token_for_slot) — never stored as
	// independent absolute truth on the node.
	uint32_t     origin_id;     // originating madc token's arena slot-id (0 = synthetic)
	DataDef     *datadef;       // madc type info (NULL if not type-related)
	const char  *typedef_name;  // source typedef alias (e.g. "EXT_BV"), NULL if none
	const char  *error_msg;     // non-NULL: error/incomplete node; a tree that
				    // contains one MUST NOT be handed to c2mir
	CirSourceLang src_lang;     // which language produced this node
	bool         synth_from_origin; // true: a lowering artifact (e.g. a ctor/dtor
				    // call synthesized for a C++ construct). Shares its
				    // origin with the high-level node; the reverse-
				    // renderer emits the origin ONCE and suppresses these.
	uint8_t      _pad[6];       // alignment padding

	// Cast to node_t for c2mir API calls
	node_t as_node() { return (node_t)&base; }

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
// Destroyed in bulk when the arena goes out of scope.

class CirArena {
	static const size_t PAGE_SIZE = 4096;  // nodes per page

	struct Page {
		cir_node nodes[PAGE_SIZE];
		size_t used;
		Page() : used(0) {}
	};

	std::vector<Page *> pages;
	std::vector<char *> strings;   // string pool for madc-owned strings

public:
	CirArena()
	{
		pages.push_back(new Page());
	}

	~CirArena()
	{
		for (auto *p : pages) delete p;
		for (auto *s : strings) delete[] s;
	}

	// Allocate a zeroed cir_node
	cir_node *alloc()
	{
		Page *p = pages.back();
		if (p->used >= PAGE_SIZE) {
			p = new Page();
			pages.push_back(p);
		}
		cir_node *cn = &p->nodes[p->used++];
		memset(cn, 0, sizeof(cir_node));
		return cn;
	}

	// Copy a string into the arena's string pool.
	// Returns a pointer valid for the arena's lifetime.
	const char *intern(const char *s)
	{
		if (!s) return NULL;
		size_t len = strlen(s) + 1;
		char *copy = new char[len];
		memcpy(copy, s, len);
		strings.push_back(copy);
		return copy;
	}

	// Copy a string with explicit length into the arena's string pool.
	const char *intern(const char *s, size_t len)
	{
		if (!s) return NULL;
		char *copy = new char[len];
		memcpy(copy, s, len);
		strings.push_back(copy);
		return copy;
	}
};

#endif // __CIR_NODE_H
