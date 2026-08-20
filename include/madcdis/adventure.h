#ifndef __MADCDIS_ADVENTURE_H
#define __MADCDIS_ADVENTURE_H 1

// madcdis/adventure.h — the PILOT-REFERENCE application layer (Track 7
// Phase 1, S4): the text-adventure verb catalog, target resolution, and
// the room projection. This is deliberately APPLICATION code — it owns
// game vocabulary (the `in`/`exit` relations; the short/desc/portable/
// fuel/lit/closed/blocks/requires/refusal/grants/dark/turn properties) the
// generic hub layers must never learn. It ships as reference code the way
// naked-objects frameworks ship a demo domain: it proves the seams
// (compiled verb catalog bound by %verb data; projections composing prose
// from facts) and is slated to migrate to script-attached verbs once
// eval/exec lands (plan, Out of scope).
//
// THREAD-SAFETY CONTRACT: pure functions over the world they are handed;
// confined with it.

#include <string>
#include <vector>

#include "madcdis/hub.h"
#include "madcdis/verbs.h"
#include "madcdis/projection.h"
#include "madcdis/world_text.h"

namespace madc {
namespace hub {
namespace adventure {

// ---- bag conveniences (application-side reads; absent = defaults) ----
inline std::string bag_str(const entity *e, const char *key)
{
    if ( e && e->bag.is_object() )
    {
	std::map<std::string, madc::value>::const_iterator it
	    = e->bag.as_object().find(key);
	if ( it != e->bag.as_object().end() && it->second.is_string() )
	    return it->second.as_string();
    }
    return std::string();
}
inline int64_t bag_int(const entity *e, const char *key, int64_t dflt = 0)
{
    if ( e && e->bag.is_object() )
    {
	std::map<std::string, madc::value>::const_iterator it
	    = e->bag.as_object().find(key);
	if ( it != e->bag.as_object().end() && it->second.is_integer() )
	    return it->second.as_integer();
    }
    return dflt;
}
inline bool bag_bool(const entity *e, const char *key)
{
    if ( e && e->bag.is_object() )
    {
	std::map<std::string, madc::value>::const_iterator it
	    = e->bag.as_object().find(key);
	if ( it != e->bag.as_object().end() && it->second.is_boolean() )
	    return it->second.as_boolean();
    }
    return false;
}
// Display name: the authored `short` ("a brass lantern") or the canonical
// spelling.
inline std::string display(const world &w, const entity *e)
{
    std::string s = bag_str(e, "short");
    return s.empty() && e ? std::string(w.spelling(e->name)) : s;
}

// ---- target resolution: carried things, then room contents, then the
// room itself; a word matches an entity's canonical name or its `noun`
// property. ----
inline entity_id resolve(const world &w, entity_id actor,
			 const std::string &word)
{
    if ( word.empty() )
	return 0;
    name_id rel_in = w.intern("in");
    std::vector<entity_id> scope = w.sources(actor, rel_in);
    entity_id room = w.target(actor, rel_in, (name_id)0);
    if ( room != 0 )
    {
	std::vector<entity_id> here = w.sources(room, rel_in);
	scope.insert(scope.end(), here.begin(), here.end());
	scope.push_back(room);
    }
    for ( size_t i = 0; i < scope.size(); ++i )
    {
	const entity *e = w.get(scope[i]);
	if ( !e )
	    continue;
	if ( word == w.spelling(e->name) || word == bag_str(e, "noun") )
	    return scope[i];
    }
    return 0;
}

// ---- the room projection (the play view). Prose composed HERE, from the
// facts; the renderer only typesets. Dark rooms show darkness unless a lit
// light source is in the room or carried. ----
inline uinode room_view(const world &w, const roles &r,
			const credentials &, entity_id actor)
{
    name_id rel_in = w.intern("in");
    name_id rel_exit = w.intern("exit");
    uinode root(r.group);
    entity_id room_id = w.target(actor, rel_in, (name_id)0);
    const entity *room = w.get(room_id);
    if ( !room )
    {
	uinode lost(r.status);
	lost.content = madc::value("You are nowhere at all.");
	root.add(lost);
	return root;
    }
    root.subject = room_id;

    std::vector<entity_id> here = w.sources(room_id, rel_in);
    if ( bag_bool(room, "dark") )
    {
	bool lit = false;
	std::vector<entity_id> carried = w.sources(actor, rel_in);
	std::vector<entity_id> candidates = here;
	candidates.insert(candidates.end(), carried.begin(), carried.end());
	for ( size_t i = 0; i < candidates.size() && !lit; ++i )
	    lit = bag_bool(w.get(candidates[i]), "lit");
	if ( !lit )
	{
	    uinode dark(r.content);
	    dark.content = madc::value(
		"It is pitch dark. You are likely to be eaten by a grue.");
	    root.add(dark);
	    return root;
	}
    }

    uinode head(r.heading);
    head.subject = room_id;
    std::string title = bag_str(room, "title");
    head.label = madc::value(title.empty() ? std::string(w.spelling(room->name))
					   : title);
    root.add(head);
    std::string desc = bag_str(room, "desc");
    if ( !desc.empty() )
    {
	uinode body(r.content);
	body.content = madc::value(desc);
	root.add(body);
    }

    uinode things(r.list);
    for ( size_t i = 0; i < here.size(); ++i )
    {
	if ( here[i] == actor )
	    continue;
	const entity *e = w.get(here[i]);
	if ( !e )
	    continue;
	uinode item(r.item);
	item.subject = here[i];
	item.content = madc::value(prose::sentence(display(w, e) + " is here"));
	things.add(item);
    }
    if ( !things.children.empty() )
	root.add(things);

    std::vector<name_id> dirs = w.link_keys(room_id, rel_exit);
    std::vector<std::string> dir_names;
    for ( size_t i = 0; i < dirs.size(); ++i )
	dir_names.push_back(w.spelling(dirs[i]));
    uinode exits(r.status);
    exits.content = madc::value(
	dir_names.empty() ? std::string("Exits: none.")
			  : "Exits: " + prose::enumerate(dir_names) + ".");
    root.add(exits);
    return root;
}

// ---- verb handlers (the compiled catalog %verb binds by name) ----

inline bool h_go(mutation_context &mc, const credentials &, entity_id actor,
		 entity_id, const std::string &arg, std::string &out)
{
    const world &w = mc.view();
    name_id rel_in = mc.intern("in");
    name_id rel_exit = mc.intern("exit");
    if ( arg.empty() )
    {
	out = "Go where?";
	return false;
    }
    entity_id room = w.target(actor, rel_in, (name_id)0);
    entity_id dest = w.target(room, rel_exit, mc.intern(arg));
    if ( dest == 0 )
    {
	out = "You can't go that way.";
	return false;
    }
    // A closed door blocking this direction bars the way.
    std::vector<entity_id> here = w.sources(room, rel_in);
    for ( size_t i = 0; i < here.size(); ++i )
    {
	const entity *e = w.get(here[i]);
	if ( bag_str(e, "blocks") == arg && bag_bool(e, "closed") )
	{
	    std::string msg = bag_str(e, "blocked_msg");
	    out = msg.empty()
		? prose::sentence(display(w, e) + " bars the way") : msg;
	    return false;
	}
    }
    mc.link_remove(actor, rel_in, room);
    mc.link_add(actor, rel_in, dest);
    out = "You go " + arg + ".";
    return true;
}

inline bool h_take(mutation_context &mc, const credentials &, entity_id actor,
		   entity_id target, const std::string &, std::string &out)
{
    const world &w = mc.view();
    name_id rel_in = mc.intern("in");
    if ( target == 0 )
    {
	out = "Take what?";
	return false;
    }
    if ( w.target(target, rel_in, (name_id)0) == actor )
    {
	out = "You already have it.";
	return false;
    }
    const entity *e = w.get(target);
    if ( !bag_bool(e, "portable") )
    {
	out = "You can't take that.";
	return false;
    }
    entity_id holder = w.target(target, rel_in, (name_id)0);
    if ( holder != 0 )
	mc.link_remove(target, rel_in, holder);
    mc.link_add(target, rel_in, actor);
    out = "Taken.";
    return true;
}

inline bool h_drop(mutation_context &mc, const credentials &, entity_id actor,
		   entity_id target, const std::string &, std::string &out)
{
    const world &w = mc.view();
    name_id rel_in = mc.intern("in");
    if ( target == 0 || w.target(target, rel_in, (name_id)0) != actor )
    {
	out = "You don't have that.";
	return false;
    }
    entity_id room = w.target(actor, rel_in, (name_id)0);
    mc.link_remove(target, rel_in, actor);
    mc.link_add(target, rel_in, room);
    out = "Dropped.";
    return true;
}

inline bool h_light(mutation_context &mc, const credentials &, entity_id,
		    entity_id target, const std::string &, std::string &out)
{
    entity *e = target ? mc.edit(target) : (entity *)0;
    if ( !e )
    {
	out = "Light what?";
	return false;
    }
    if ( bag_int(e, "fuel", 0) <= 0 )
    {
	out = prose::sentence(display(mc.view(), e) + " is out of fuel");
	return false;
    }
    e->bag.object()["lit"] = madc::value(true);
    out = prose::sentence(display(mc.view(), e) + " glows warmly");
    return true;
}

inline bool h_douse(mutation_context &mc, const credentials &, entity_id,
		    entity_id target, const std::string &, std::string &out)
{
    entity *e = target ? mc.edit(target) : (entity *)0;
    if ( !e || !bag_bool(e, "lit") )
    {
	out = "It isn't lit.";
	return false;
    }
    e->bag.object()["lit"] = madc::value(false);
    out = "Extinguished.";
    return true;
}

// The entity-attached condition case (gate G3): an openable thing may
// carry `requires = <key>`; the key is checked against the invoker's
// credentials — which include inventory-derived grants, so holding the
// brass key IS the permission.
inline bool h_open(mutation_context &mc, const credentials &creds, entity_id,
		   entity_id target, const std::string &, std::string &out)
{
    entity *e = target ? mc.edit(target) : (entity *)0;
    if ( !e || !e->bag.is_object()
      || e->bag.as_object().count("closed") == 0 )
    {
	out = "You can't open that.";
	return false;
    }
    if ( !bag_bool(e, "closed") )
    {
	out = "It is already open.";
	return false;
    }
    std::string needs = bag_str(e, "requires");
    if ( !needs.empty() )
    {
	requirement req;
	req.keys.push_back(mc.intern(needs));
	if ( !req.satisfied_by(creds) )
	{
	    std::string msg = bag_str(e, "refusal");
	    out = msg.empty() ? "It is locked." : msg;
	    return false;
	}
    }
    e->bag.object()["closed"] = madc::value(false);
    out = prose::sentence(display(mc.view(), e) + " swings open");
    return true;
}

inline bool h_inventory(mutation_context &mc, const credentials &,
			entity_id actor, entity_id, const std::string &,
			std::string &out)
{
    const world &w = mc.view();
    std::vector<entity_id> carried = w.sources(actor, mc.intern("in"));
    std::vector<std::string> names;
    for ( size_t i = 0; i < carried.size(); ++i )
	names.push_back(display(w, w.get(carried[i])));
    out = names.empty() ? "You are carrying nothing."
			: prose::sentence("you are carrying "
					  + prose::enumerate(names));
    return true;
}

// The builder's verb (%verb edit key=builder ...): `edit <target-word>
// <prop> <value...>` — sets one bag property, kinds inferred by the same
// rule the world format uses.
inline bool h_edit(mutation_context &mc, const credentials &, entity_id,
		   entity_id target, const std::string &arg, std::string &out)
{
    entity *e = target ? mc.edit(target) : (entity *)0;
    if ( !e )
    {
	out = "Edit what?";
	return false;
    }
    std::vector<std::string> words = detail::wt_words(arg);
    if ( words.size() < 2 )
    {
	out = "edit needs: <prop> <value>";
	return false;
    }
    std::string prop = words[0];
    std::string text = arg.substr(arg.find(prop) + prop.size());
    e->bag.object()[prop] = detail::wt_value(detail::wt_trim(text));
    out = "Set " + prop + ".";
    return true;
}

// ---- catalog binding: %verb NAME -> compiled handler; the requirement
// and refusal arrive from the declaration (data binds code by NAME). ----
inline bool register_catalog(verb_table &t, world &w,
			     const world_doc::verb_decl &decl,
			     std::string &err)
{
    verb_handler fn = (verb_handler)0;
    if ( decl.name == "go" )		fn = h_go;
    else if ( decl.name == "take" )	fn = h_take;
    else if ( decl.name == "drop" )	fn = h_drop;
    else if ( decl.name == "light" )	fn = h_light;
    else if ( decl.name == "douse" )	fn = h_douse;
    else if ( decl.name == "open" )	fn = h_open;
    else if ( decl.name == "inventory" ) fn = h_inventory;
    else if ( decl.name == "edit" )	fn = h_edit;
    if ( !fn )
    {
	err = "no compiled handler named `" + decl.name + "`";
	return false;
    }
    requirement req;
    for ( size_t i = 0; i < decl.keys.size(); ++i )
	req.keys.push_back(w.intern(decl.keys[i]));
    if ( !decl.domain.empty() )
    {
	req.level_domain = w.intern(decl.domain);
	req.min_level = (int32_t)decl.min_level;
    }
    t.register_verb(w.intern(decl.name), req, decl.refusal, fn);
    return true;
}

// ---- the turn tick: the world's own time passing (state evolution).
// Convention: an entity named `world-state` carries `turn`; every lit
// fueled thing burns one unit, dying to unlit at zero. ----
inline void tick(world &w)
{
    entity_id state = w.find("world-state");
    if ( state != 0 )
    {
	entity *s = w.get(state);
	s->bag.object()["turn"] =
	    madc::value(bag_int(s, "turn", 0) + 1);
    }
    for ( entity_id id = 1; id <= (entity_id)w.entity_count(); ++id )
    {
	entity *e = w.get(id);
	if ( !e || !bag_bool(e, "lit") )
	    continue;
	if ( e->bag.as_object().count("fuel") == 0 )
	    continue;	// eternal lights burn nothing
	int64_t fuel = bag_int(e, "fuel", 0) - 1;
	e->bag.object()["fuel"] = madc::value(fuel > 0 ? fuel : (int64_t)0);
	if ( fuel <= 0 )
	    e->bag.object()["lit"] = madc::value(false);
    }
}

} // namespace adventure
} // namespace hub
} // namespace madc

#endif // __MADCDIS_ADVENTURE_H
