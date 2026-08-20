#ifndef __MADCDIS_HUB_H
#define __MADCDIS_HUB_H 1

// madcdis/hub.h — the data-hub entity/component substrate and the
// keys/levels access model (Track 7 Phase 1, slice S1).
//
//   design: docs/plans/2026-08-20-data-hub-projection-rendering.md
//   plan:   docs/plans/2026-08-20-track7-phase1-text-adventure.md
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a hub::world and
// everything reached through it are CONFINED TO ONE THREAD. The shapes are
// concurrency-ready per design demand 15 — queries return ids or copies,
// mutation happens through world methods (narrowed further to the verb
// layer's mutation context in S2) — so a future multi-threaded hub changes
// contracts, not signatures.
//
// THE ENTITY/COMPONENT READING (design, decided at owner review): an entity
// is an IDENTITY; typed components attach dynamically; a classic record is
// the degenerate one-component case. Phase 1 ships one component kind — the
// madc::value property BAG (object kind, vivified on first use) — plus
// keyed entity->entity LINKS: the graph_edge relation at ENTITY granularity.
// madc::Relation<A,B> (madcdis/relation.h) is the DATASET-granularity
// sibling and remains the owner of storage-facing joins; it is not reused
// here because its endpoints are typed record datasets, not identities.
//
// THE ACCESS MODEL (owner-specified 2026-08-20): access to anything is a
// CONDITION over CREDENTIALS. Two primitives, combinable:
//   keys   — binary possession; textual at the surface, interned name-ids
//            with the holder's set as a bitset underneath. Keys LAYER: a
//            key->key implication relation ("a master key for a domain")
//            whose transitive closure is folded into the bitset when
//            credentials are built, so checks stay one mask test.
//   levels — numeric per DOMAIN; the condition is level(domain) >= n.
// A role IS a key. Credentials also derive from data: an entity carried in
// a containment closure whose bag names a grant confers that key — one
// machinery for a game door and an admin screen.
//
// Scale contract: link and name scans are linear — the pilot's worlds are
// tens of entities. The API is the contract; indexing is a later, measured
// change (never assume this container is the bottleneck without callgrind).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "madcdis/id_table.h"
#include "madcdis/intern_table.h"
#include "libmadc/value.h"

namespace madc {
namespace hub {

typedef uint32_t entity_id;	// 0 = none (id_table segment base is 1)
typedef uint32_t name_id;	// interned spelling: names, keys, domains, rels

// ---------------------------------------------------------------- credentials
// What a principal HOLDS: a key bitset (bit index = interned key id) and
// per-domain levels. Built per session/actor, checked against requirements.
class credentials
{
    std::vector<uint64_t>     _keybits;
    std::map<name_id, int32_t> _levels;
public:
    // Absent-domain level: the identity that fails every `>= n` gate a
    // requirement can legally express. Typed enum (prvalue) — no ODR
    // definition needed (the intern_table NIL idiom).
    enum : int32_t { no_level = INT32_MIN };

    void grant_key(name_id k)
    {
	if ( k == 0 )
	    return;
	size_t word = k >> 6;
	if ( word >= _keybits.size() )
	    _keybits.resize(word + 1, 0);
	_keybits[word] |= (uint64_t)1 << (k & 63);
    }
    bool has_key(name_id k) const
    {
	if ( k == 0 )
	    return false;
	size_t word = k >> 6;
	return word < _keybits.size()
	    && (_keybits[word] & ((uint64_t)1 << (k & 63))) != 0;
    }
    void set_level(name_id domain, int32_t lvl)
    {
	if ( domain != 0 )
	    _levels[domain] = lvl;
    }
    int32_t level(name_id domain) const
    {
	std::map<name_id, int32_t>::const_iterator it = _levels.find(domain);
	return it == _levels.end() ? no_level : it->second;
    }
    size_t key_count() const
    {
	size_t n = 0;
	for ( size_t w = 0; w < _keybits.size(); ++w )
	    for ( uint64_t bits = _keybits[w]; bits; bits &= bits - 1 )
		++n;
	return n;
    }
};

// ---------------------------------------------------------------- requirement
// The CONDITION on a protected thing (verb, projection, field, door): every
// listed key must be held AND the optional (domain, min) level must be met.
// Boolean-combined requirement expressions arrive later, as data (design,
// Decided) — this is the Phase 1 form.
struct requirement
{
    std::vector<name_id> keys;
    name_id level_domain;
    int32_t min_level;

    requirement() : level_domain(0), min_level(0) {}

    bool empty() const { return keys.empty() && level_domain == 0; }
    bool satisfied_by(const credentials &c) const
    {
	for ( size_t i = 0; i < keys.size(); ++i )
	    if ( !c.has_key(keys[i]) )
		return false;
	if ( level_domain != 0 && c.level(level_domain) < min_level )
	    return false;
	return true;
    }
};

// --------------------------------------------------------------------- entity
// An identity + the Phase 1 component: a value property bag (object kind,
// vivified by madc::value::object() on first write). Entities live for the
// life of the world (id_table contract: ids are permanent, never renumbered).
struct entity
{
    entity_id   id;
    name_id     name;	// canonical spelling ("brass-lantern")
    madc::value bag;

    entity() : id(0), name(0) {}
};

// ----------------------------------------------------------------------- link
// A keyed, directed entity->entity edge: (from, rel, key, to). `key`
// disambiguates within one relation kind (an exit's direction); 0 = unkeyed
// (containment). Entity-granularity graph_edge — see the header comment.
struct link
{
    entity_id from;
    name_id   rel;
    name_id   key;
    entity_id to;
    link(entity_id f, name_id r, name_id k, entity_id t)
	: from(f), rel(r), key(k), to(t) {}
};

// ---------------------------------------------------------------------- world
class world
{
    dis::intern_table		    _names;
    dis::id_table<entity>	    _entities;	// base 1: id 0 = none
    std::vector<entity *>	    _owned;
    std::vector<link>		    _links;
    std::map<name_id, std::vector<name_id> > _implies;	// key -> keys it provides

    world(const world &);		// identities are not copyable
    world &operator=(const world &);
public:
    world() : _entities(1) {}
    ~world()
    {
	for ( size_t i = 0; i < _owned.size(); ++i )
	    delete _owned[i];
    }

    // --- names: one interned namespace for entity names, keys, domains,
    // relation kinds. Interning is how a spelling becomes checkable in O(1).
    name_id intern(const std::string &s) { return _names.intern(s); }
    name_id intern(const char *s)        { return _names.intern(s); }
    const char *spelling(name_id id) const { return _names.c_str(id); }

    // --- entities
    entity_id create(const std::string &name)
    {
	entity *e = new entity();
	e->name = _names.intern(name);
	e->id = _entities.add(e);
	_owned.push_back(e);
	return e->id;
    }
    entity *get(entity_id id)             { return _entities.get(id); }
    const entity *get(entity_id id) const { return _entities.get(id); }
    // First entity whose canonical name interns equal (linear; pilot scale).
    entity_id find(const std::string &name) const
    {
	uint32_t want = _names.intern(name);
	for ( size_t i = 0; i < _owned.size(); ++i )
	    if ( _owned[i]->name == (name_id)want )
		return _owned[i]->id;
	return 0;
    }
    size_t entity_count() const { return _owned.size(); }

    // --- links
    void link_add(entity_id from, name_id rel, entity_id to, name_id key = 0)
    {
	_links.push_back(link(from, rel, key, to));
    }
    // Remove the first (from, rel, to) edge regardless of key. True if found.
    bool link_remove(entity_id from, name_id rel, entity_id to)
    {
	for ( size_t i = 0; i < _links.size(); ++i )
	    if ( _links[i].from == from && _links[i].rel == rel
	      && _links[i].to == to )
	    {
		_links.erase(_links.begin() + i);
		return true;
	    }
	return false;
    }
    // Keyed single target: the exit-north case. 0 when absent.
    entity_id target(entity_id from, name_id rel, name_id key) const
    {
	for ( size_t i = 0; i < _links.size(); ++i )
	    if ( _links[i].from == from && _links[i].rel == rel
	      && _links[i].key == key )
		return _links[i].to;
	return 0;
    }
    std::vector<entity_id> targets(entity_id from, name_id rel) const
    {
	std::vector<entity_id> out;
	for ( size_t i = 0; i < _links.size(); ++i )
	    if ( _links[i].from == from && _links[i].rel == rel )
		out.push_back(_links[i].to);
	return out;
    }
    // Reverse: all `from`s pointing at `to` — a container's contents when
    // rel is the application's containment kind.
    std::vector<entity_id> sources(entity_id to, name_id rel) const
    {
	std::vector<entity_id> out;
	for ( size_t i = 0; i < _links.size(); ++i )
	    if ( _links[i].to == to && _links[i].rel == rel )
		out.push_back(_links[i].from);
	return out;
    }
    // Keys of the keyed links from `from` (the exit-listing case), in
    // insertion order.
    std::vector<name_id> link_keys(entity_id from, name_id rel) const
    {
	std::vector<name_id> out;
	for ( size_t i = 0; i < _links.size(); ++i )
	    if ( _links[i].from == from && _links[i].rel == rel
	      && _links[i].key != 0 )
		out.push_back(_links[i].key);
	return out;
    }
    // Every link touching an entity, either end — the inspector's and the
    // exporter's enumeration (copies; the store stays private).
    std::vector<link> links_of(entity_id e) const
    {
	std::vector<link> out;
	for ( size_t i = 0; i < _links.size(); ++i )
	    if ( _links[i].from == e || _links[i].to == e )
		out.push_back(_links[i]);
	return out;
    }
    std::vector<link> all_links() const { return _links; }

    // --- key layering: `master` provides `granted` (and, transitively,
    // whatever `granted` provides). Folded at credential build; cycle-safe.
    void key_implies(name_id master, name_id granted)
    {
	if ( master != 0 && granted != 0 )
	    _implies[master].push_back(granted);
    }
    void close_over_implications(credentials &c) const
    {
	// BFS from every held key; the bitset itself is the visited set.
	std::vector<name_id> work;
	for ( std::map<name_id, std::vector<name_id> >::const_iterator it
		= _implies.begin(); it != _implies.end(); ++it )
	    if ( c.has_key(it->first) )
		work.push_back(it->first);
	while ( !work.empty() )
	{
	    name_id k = work.back();
	    work.pop_back();
	    std::map<name_id, std::vector<name_id> >::const_iterator it
		= _implies.find(k);
	    if ( it == _implies.end() )
		continue;
	    for ( size_t i = 0; i < it->second.size(); ++i )
	    {
		name_id g = it->second[i];
		if ( !c.has_key(g) )
		{
		    c.grant_key(g);
		    work.push_back(g);
		}
	    }
	}
    }

    // --- data-derived credentials: session grants + keys conferred by the
    // actor's containment closure. `held_rel` is the APPLICATION'S
    // containment relation kind and `grants_prop` its bag property naming a
    // conferred key — parameters, never hardwired spellings (generic
    // machinery carries no application vocabulary). The walk is transitive
    // (a key inside a carried sack still counts) and cycle-safe.
    credentials credentials_for(entity_id actor, const credentials &session,
				name_id held_rel, const char *grants_prop) const
    {
	credentials out = session;
	std::vector<entity_id> work;
	std::vector<bool> seen((size_t)_entities.base() + _entities.size(), false);
	work.push_back(actor);
	while ( !work.empty() )
	{
	    entity_id container = work.back();
	    work.pop_back();
	    std::vector<entity_id> held = sources(container, held_rel);
	    for ( size_t i = 0; i < held.size(); ++i )
	    {
		entity_id hid = held[i];
		if ( (size_t)hid < seen.size() && seen[hid] )
		    continue;
		if ( (size_t)hid < seen.size() )
		    seen[hid] = true;
		const entity *e = get(hid);
		if ( !e )
		    continue;
		if ( e->bag.is_object() )
		{
		    std::map<std::string, madc::value>::const_iterator it
			= e->bag.as_object().find(grants_prop);
		    if ( it != e->bag.as_object().end()
		      && it->second.is_string() )
			out.grant_key(_names.intern(it->second.as_string()));
		}
		work.push_back(hid);	// carried containers carry on
	    }
	}
	close_over_implications(out);
	return out;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_HUB_H
