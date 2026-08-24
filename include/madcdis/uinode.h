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

#include <map>
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
    name_id choice;	// a menu: the node's children are its OPTIONS

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
	r.choice = w.intern("choice");
	return r;
    }
};

// The projection tree AS hub data (design demand 3): a uinode spelled
// into an ordinary value tree — role/states/actions by NAME (a raw id is
// never script-facing; names are the stable identity), subject as the
// entity handle. Sparse: only meaningful fields appear, so script walks
// stay php-shaped. Schema: { role, label, content, hints, states[],
// actions[], subject, children[] }.
inline madc::value uinode_to_value(const world &w, const uinode &n)
{
    madc::value v = madc::value::make_object();
    std::map<std::string, madc::value> &o = v.object();
    o["role"] = madc::value(std::string(n.role ? w.spelling(n.role) : ""));
    if ( !n.label.is_null() )
	o["label"] = n.label;
    if ( !n.content.is_null() )
	o["content"] = n.content;
    if ( !n.hints.is_null() )
	o["hints"] = n.hints;
    if ( !n.states.empty() )
    {
	madc::value a = madc::value::make_array();
	for ( size_t i = 0; i < n.states.size(); ++i )
	    a.array().push_back(
		madc::value(std::string(w.spelling(n.states[i]))));
	o["states"] = a;
    }
    if ( !n.actions.empty() )
    {
	madc::value a = madc::value::make_array();
	for ( size_t i = 0; i < n.actions.size(); ++i )
	    a.array().push_back(
		madc::value(std::string(w.spelling(n.actions[i]))));
	o["actions"] = a;
    }
    if ( n.subject != 0 )
	o["subject"] = madc::value((int64_t)n.subject);
    if ( !n.children.empty() )
    {
	madc::value a = madc::value::make_array();
	for ( size_t i = 0; i < n.children.size(); ++i )
	    a.array().push_back(uinode_to_value(w, n.children[i]));
	o["children"] = a;
    }
    return v;
}

// The inverse: a value-shaped tree (the schema above; every field
// optional) interned back into a uinode — what lets an APPLICATION
// compose a projection as ordinary data and hand it to any renderer
// (projection-as-data). Tolerant reader: a wrong-kind field is skipped;
// an absent role yields 0 (structure-only for the renderer); a bare
// non-object value is the smallest useful leaf — plain content.
inline uinode value_to_uinode(const world &w, const madc::value &v)
{
    uinode n;
    if ( !v.is_object() )
    {
	n.content = v;
	return n;
    }
    const std::map<std::string, madc::value> &o = v.as_object();
    std::map<std::string, madc::value>::const_iterator it;
    if ( (it = o.find("role")) != o.end() && it->second.is_string() )
	n.role = w.intern(it->second.as_string());
    if ( (it = o.find("label")) != o.end() )
	n.label = it->second;
    if ( (it = o.find("content")) != o.end() )
	n.content = it->second;
    if ( (it = o.find("hints")) != o.end() )
	n.hints = it->second;
    if ( (it = o.find("states")) != o.end() && it->second.is_array() )
    {
	const std::vector<madc::value> &a = it->second.as_array();
	for ( size_t i = 0; i < a.size(); ++i )
	    if ( a[i].is_string() )
		n.states.push_back(w.intern(a[i].as_string()));
    }
    if ( (it = o.find("actions")) != o.end() && it->second.is_array() )
    {
	const std::vector<madc::value> &a = it->second.as_array();
	for ( size_t i = 0; i < a.size(); ++i )
	    if ( a[i].is_string() )
		n.actions.push_back(w.intern(a[i].as_string()));
    }
    if ( (it = o.find("subject")) != o.end() && it->second.is_integer() )
	n.subject = (entity_id)it->second.as_integer();
    if ( (it = o.find("children")) != o.end() && it->second.is_array() )
    {
	const std::vector<madc::value> &a = it->second.as_array();
	for ( size_t i = 0; i < a.size(); ++i )
	    n.children.push_back(value_to_uinode(w, a[i]));
    }
    return n;
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UINODE_H
