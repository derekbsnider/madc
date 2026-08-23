#ifndef __MADCDIS_UINODE_H
#define __MADCDIS_UINODE_H 1

// madcdis/uinode.h — the value-typed semantic tree (Track 7 Phase 1, S3):
// the ONE presentation IR every projection produces and every renderer
// consumes, from the level-0 teletype to a GPU scene mapper.
//
//   design: docs/plans/2026-08-20-data-hub-projection-rendering.md
//   plan:   docs/plans/2026-08-20-track7-phase1-text-adventure.md
//
// VALUE-FIRST (owner directive): content — label, body, hints — is
// madc::value; classification — roles, states, action kinds — is a
// registry-interned name_id (enum-fast at every hot boundary, runtime-
// extensible, never a string compare in a render loop). The tree is
// thereby ordinary hub data: storable, projectable, diffable by the same
// machinery as everything else.
//
// `subject` threads the projected entity's identity through the tree —
// the hook the builder inspector uses today and per-connection diffing
// (design 7.3) keys on later.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a uinode tree
// is a plain value object — confined to the thread that builds it; two
// trees never share state (value deep-copies).

#include <string>
#include <vector>

#include "madcdis/hub.h"

namespace madc {
namespace hub {

struct uinode
{
    name_id		 role;		// interned classification; never 0 in a real node
    std::vector<name_id> states;	// interned state flags (SELECTED, DISABLED, ...)
    madc::value		 label;		// short name/title
    madc::value		 content;	// body content
    madc::value		 hints;		// optional object-kind bag (size, weight, ...)
    std::vector<name_id> actions;	// verb name-ids invocable on this node
    entity_id		 subject;	// the entity this node projects; 0 = none
    std::vector<uinode>	 children;

    uinode() : role(0), subject(0) {}
    explicit uinode(name_id r) : role(r), subject(0) {}

    // Append a child. Returns nothing on purpose: a reference into
    // `children` would dangle across the next append (vector growth).
    void add(const uinode &child) { children.push_back(child); }
};

// The standard role vocabulary, interned once per world namespace. The
// spellings are the registry; applications may intern further roles — the
// vocabulary is extensible by construction (design demand: never a closed
// C enum a user cannot add to).
struct roles
{
    name_id heading;
    name_id content;
    name_id list;
    name_id item;
    name_id action;
    name_id status;
    name_id group;
    name_id separator;

    static roles standard(world &w)
    {
	roles r;
	r.heading = w.intern("heading");
	r.content = w.intern("content");
	r.list = w.intern("list");
	r.item = w.intern("item");
	r.action = w.intern("action");
	r.status = w.intern("status");
	r.group = w.intern("group");
	r.separator = w.intern("separator");
	return r;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UINODE_H
