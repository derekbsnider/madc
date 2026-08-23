#ifndef __MADCDIS_PROJECTION_H
#define __MADCDIS_PROJECTION_H 1

// madcdis/projection.h — the projection layer (Track 7 Phase 1, S3): for
// access-role R and purpose P, project hub data into a semantic tree.
// PROJECTION IS THE SECURITY BOUNDARY (design demand 5): a tree handed to
// any renderer already contains only what these credentials may see —
// project THEN transmit, never filter as a renderer courtesy.
//
// Phase 1 projections are code-defined (design, Decided): a projection is
// a function of (world, roles, credentials, focus) — internally the shape
// is query + mapping, so the stored-view descriptor later serializes the
// query half verbatim and names the mapping half.
//
// inspect() is the generic entity browser — bag properties plus links,
// straight off the metadata — the first, minimal naked-objects moment
// (the builder's debug view in the pilot). It speaks only hub vocabulary
// (properties, links), never application vocabulary; WHO may see it is
// the caller's projection-selection decision, gated by credentials like
// every projection.
//
// THREAD-SAFETY CONTRACT: pure functions of their arguments; trees are
// plain values.

#include <string>
#include <vector>

#include "madcdis/uinode.h"
#include "madcdis/prose.h"

namespace madc {
namespace hub {

// The Phase 1 projection shape. `focus` is the projection's subject (the
// room being viewed, the entity being inspected); the projection reads
// whatever else it queries from the world.
typedef uinode (*projection_fn)(const world &w, const roles &r,
				const credentials &creds, entity_id focus);

// One property line of the inspector: scalar payloads spell themselves;
// container kinds name their kind (the deep walk is php::print_r's job,
// not the inspector's).
inline std::string inspect_property_text(const madc::value &v)
{
    if ( v.is_object() || v.is_array() || v.is_instance() )
	return std::string("(") + madc::value::kind_name(v.type()) + ")";
    return prose::text_of(v);
}

inline uinode inspect(const world &w, const roles &r, entity_id id)
{
    uinode root(r.group);
    root.subject = id;
    const entity *e = w.get(id);
    if ( !e )
    {
	uinode missing(r.status);
	missing.content = madc::value("no such entity");
	root.add(missing);
	return root;
    }

    uinode head(r.heading);
    head.subject = id;
    head.label = madc::value(std::string(w.spelling(e->name)));
    root.add(head);

    uinode props(r.list);
    props.label = madc::value("properties");
    if ( e->bag.is_object() )
    {
	const std::map<std::string, madc::value> &bag = e->bag.as_object();
	for ( std::map<std::string, madc::value>::const_iterator it
		= bag.begin(); it != bag.end(); ++it )
	{
	    uinode item(r.item);
	    item.label = madc::value(it->first);
	    item.content = madc::value(it->first + " = "
				       + inspect_property_text(it->second));
	    props.add(item);
	}
    }
    root.add(props);

    uinode edges(r.list);
    edges.label = madc::value("links");
    std::vector<link> ls = w.links_of(id);
    for ( size_t i = 0; i < ls.size(); ++i )
    {
	const entity *from = w.get(ls[i].from);
	const entity *to = w.get(ls[i].to);
	std::string line = std::string(from ? w.spelling(from->name) : "?")
	    + " -" + w.spelling(ls[i].rel);
	if ( ls[i].key != 0 )
	    line += std::string("[") + w.spelling(ls[i].key) + "]";
	line += std::string("-> ") + (to ? w.spelling(to->name) : "?");
	uinode item(r.item);
	item.content = madc::value(line);
	edges.add(item);
    }
    root.add(edges);
    return root;
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_PROJECTION_H
