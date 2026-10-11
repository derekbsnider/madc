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
#include "madcdis/text_buffer.h"
#include "madcdis/document.h"
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
    name_id     name;	// canonical spelling ("oil-lamp")
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
    std::map<entity_id, text_buffer> _texts;	// sparse text components
    std::map<entity_id, document>    _docs;	// sparse document components
    std::vector<text_listener *>     _listeners;	// the edit notification's
    bool                             _publishing;	// a delivery is in progress

    world(const world &);		// identities are not copyable
    world &operator=(const world &);

    // THE ONE PUBLISH POINT (split arc 1A.1, design §2.2): every text
    // mutator below ends here. The document's anchors shift FIRST, so a
    // listener reads shifted positions; then every listener, in order,
    // with mutation locked (_publishing) — a mutator called inside a
    // listener returns false.
    void publish(text_change &c)
    {
	std::map<entity_id, document>::iterator d = _docs.find(c.entity);
	if ( d != _docs.end() )
	{
	    if ( c.what == text_change::kind::splice )
		d->second.shift(c.off, c.old_len, c.new_len);
	    else	// load: clamp; undo / redo: the caller restored the
		d->second.clamp(c.new_len);	// snapshot's, the rest clamp
	}
	if ( _listeners.empty() )
	    return;
	_publishing = true;
	for ( size_t i = 0; i < _listeners.size(); ++i )
	    _listeners[i]->on_text_change(*this, c);
	_publishing = false;
    }
    // A mutation is admitted when no delivery is in progress and the
    // entity is not a read-only document (a load — a reload from disk — is
    // admitted on a read-only document; the command layer gates saving).
    bool admit(entity_id id, bool is_load) const
    {
	if ( _publishing )
	    return false;
	if ( is_load )
	    return true;
	std::map<entity_id, document>::const_iterator d = _docs.find(id);
	return d == _docs.end() || !d->second.read_only();
    }
    bool whole_change(entity_id id, text_change::kind what, size_t before)
    {
	text_change c;
	c.entity = id;
	c.what = what;
	c.old_len = before;
	c.new_len = _texts[id].size();
	c.version = _texts[id].version();
	publish(c);
	return true;
    }
public:
    world() : _entities(1), _publishing(false) {}
    ~world()
    {
	for ( size_t i = 0; i < _owned.size(); ++i )
	    delete _owned[i];
    }

    // --- names: one interned namespace for entity names, keys, domains,
    // relation kinds. Interning is how a spelling becomes checkable in
    // O(1). Const like the intern table itself: a memoizing dedup adds a
    // spelling but changes no observable state.
    name_id intern(const std::string &s) const { return _names.intern(s); }
    name_id intern(const char *s)        const { return _names.intern(s); }
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
	name_id want = intern(name);
	for ( size_t i = 0; i < _owned.size(); ++i )
	    if ( _owned[i]->name == want )
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

    // --- text component (Track 7.2 R4; Track 8.1 pulled forward): the
    // hub's SECOND component kind — a piece-table buffer attached
    // SPARSELY by entity id ("an editor buffer = entity with a
    // piece-table component, natively" — design demand 7). Mutators
    // mirror through mutation_context like every write; reads are const.
    // RUNTIME-ONLY today: world_save does not carry it — a document's
    // text persists in the document's own file, written by the
    // application's save verb.
    //
    // Every mutator PUBLISHES (split arc 1A.1): one text_change record to
    // the world's listeners, after the change — see publish() above. Each
    // returns false when refused: a delivery in progress, or a read-only
    // document (a load is admitted on one). A no-op (an empty insert, an
    // erase of nothing, a replace of nothing by nothing) is admitted and
    // publishes nothing.
    bool text_load(entity_id id, const std::string &s)
    {
	if ( !admit(id, true) )
	    return false;
	size_t before = _texts[id].size();
	_texts[id].load(s);
	return whole_change(id, text_change::kind::load, before);
    }
    bool text_insert(entity_id id, size_t off, const std::string &s)
    {
	if ( !admit(id, false) )
	    return false;
	if ( s.empty() )
	    return true;
	text_buffer &b = _texts[id];
	if ( off > b.size() )
	    off = b.size();
	b.insert(off, s);
	text_change c;
	c.entity = id;
	c.version = b.version();
	c.off = off;
	c.new_len = s.size();
	c.inserted = &s;
	publish(c);
	return true;
    }
    bool text_erase(entity_id id, size_t off, size_t len)
    {
	if ( !admit(id, false) )
	    return false;
	text_buffer &b = _texts[id];
	if ( off >= b.size() || len == 0 )
	    return true;
	if ( len > b.size() - off )
	    len = b.size() - off;
	b.erase(off, len);
	text_change c;
	c.entity = id;
	c.version = b.version();
	c.off = off;
	c.old_len = len;
	publish(c);
	return true;
    }
    // ONE record for a replace (an anchor inside the range lands once).
    bool text_replace(entity_id id, size_t off, size_t len,
		      const std::string &s)
    {
	if ( !admit(id, false) )
	    return false;
	text_buffer &b = _texts[id];
	if ( off > b.size() )
	    off = b.size();
	if ( len > b.size() - off )
	    len = b.size() - off;
	if ( len == 0 && s.empty() )
	    return true;
	b.replace(off, len, s);
	text_change c;
	c.entity = id;
	c.version = b.version();
	c.off = off;
	c.old_len = len;
	c.new_len = s.size();
	c.inserted = &s;
	publish(c);
	return true;
    }
    uint64_t text_version(entity_id id) const
    {
	std::map<entity_id, text_buffer>::const_iterator it = _texts.find(id);
	return it == _texts.end() ? 0 : it->second.version();
    }
    // History (madcide IDE-2): a checkpoint snapshots the buffer state
    // with an OPAQUE application payload; undo restores the top snapshot
    // and hands the payload back (false: no component or empty history).
    // Both are writes — they mutate the component's state. An undo or
    // redo publishes a whole-buffer record. On a DOCUMENT, use the doc_*
    // forms below, which ride the anchors and the saved version in the
    // payload; these raw forms are the text-only component's.
    bool text_checkpoint(entity_id id, const madc::value &meta)
    {
	if ( _publishing )
	    return false;
	_texts[id].checkpoint(meta);
	return true;
    }
    bool text_undo(entity_id id, madc::value &meta_out)
    {
	std::map<entity_id, text_buffer>::iterator it = _texts.find(id);
	if ( it == _texts.end() || !admit(id, false) )
	    return false;
	size_t before = it->second.size();
	if ( !it->second.undo(meta_out) )
	    return false;
	return whole_change(id, text_change::kind::undo, before);
    }
    // The redo-preserving forms (madcide v2): now_meta = the payload live
    // on the document being LEFT, captured onto the opposite stack so
    // redo/undo restore document + interaction state together.
    bool text_undo(entity_id id, madc::value &meta_out,
		   const madc::value &now_meta)
    {
	std::map<entity_id, text_buffer>::iterator it = _texts.find(id);
	if ( it == _texts.end() || !admit(id, false) )
	    return false;
	size_t before = it->second.size();
	if ( !it->second.undo(meta_out, now_meta) )
	    return false;
	return whole_change(id, text_change::kind::undo, before);
    }
    bool text_redo(entity_id id, madc::value &meta_out,
		   const madc::value &now_meta)
    {
	std::map<entity_id, text_buffer>::iterator it = _texts.find(id);
	if ( it == _texts.end() || !admit(id, false) )
	    return false;
	size_t before = it->second.size();
	if ( !it->second.redo(meta_out, now_meta) )
	    return false;
	return whole_change(id, text_change::kind::redo, before);
    }

    // --- the edit notification (split arc 1A.1, design §2.2). Listeners
    // are not owned; a listener unsubscribes before it dies. Per world,
    // session-thread.
    void text_subscribe(text_listener *l)
    {
	if ( l )
	    _listeners.push_back(l);
    }
    bool text_unsubscribe(text_listener *l)
    {
	for ( size_t i = 0; i < _listeners.size(); ++i )
	    if ( _listeners[i] == l )
	    {
		_listeners.erase(_listeners.begin() + i);
		return true;
	    }
	return false;
    }
    bool publishing() const { return _publishing; }

    // --- document component (split arc 1A.1; madcdis/document.h): the
    // hub's THIRD component kind, sparse like the text. doc_attach makes an
    // entity a document (its text component is made too, empty, if
    // absent) with the loaded text as its saved state; doc_detach drops
    // the document AND its text. Reads are const; the setters are writes
    // (mutation_context mirrors them). The parse slot, the kind and the
    // disk I/O are the publics' — see the design's layering rule.
    bool doc_attach(entity_id id, const std::string &path, int64_t kind,
		    const std::string &text)
    {
	if ( _publishing || _docs.count(id) )
	    return false;
	_texts[id].load(text);
	document &d = _docs[id];
	d.set_path(path);
	d.set_kind(kind);
	d.mark_saved(_texts[id].version());
	return whole_change(id, text_change::kind::load, 0);
    }
    bool doc_detach(entity_id id)
    {
	if ( _publishing )
	    return false;
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() )
	    return false;
	_docs.erase(d);
	_texts.erase(id);
	return true;
    }
    const document *doc_of(entity_id id) const
    {
	std::map<entity_id, document>::const_iterator d = _docs.find(id);
	return d == _docs.end() ? (const document *)0 : &d->second;
    }
    bool has_doc(entity_id id) const { return _docs.count(id) != 0; }
    // The ring: every document, in entity (creation) order.
    std::vector<entity_id> docs() const
    {
	std::vector<entity_id> out;
	for ( std::map<entity_id, document>::const_iterator d = _docs.begin();
	      d != _docs.end(); ++d )
	    out.push_back(d->first);
	return out;
    }
    bool doc_set_path(entity_id id, const std::string &path, int64_t kind)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() )
	    return false;
	d->second.set_path(path);
	d->second.set_kind(kind);
	return true;
    }
    bool doc_set_read_only(entity_id id, bool ro)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() )
	    return false;
	d->second.set_read_only(ro);
	return true;
    }
    bool doc_mark_saved(entity_id id)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() )
	    return false;
	d->second.mark_saved(text_version(id));
	return true;
    }
    bool doc_modified(entity_id id) const
    {
	const document *d = doc_of(id);
	return d && d->modified(text_version(id));
    }
    bool doc_set_parse(entity_id id, int64_t handle, uint64_t version)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() )
	    return false;
	d->second.set_parse(handle, version);
	return true;
    }
    bool doc_set_parse_tu(entity_id id, const ProjectTU &tu)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() )
	    return false;
	d->second.set_parse_tu(tu);
	return true;
    }
    // The document-level history (design §2.4): the payload rides the
    // application's meta under "app" plus the anchors and the saved
    // version, so undo restores the document, its anchors and whether it
    // was clean, and hands the application's meta back. `app_now` is the
    // application's payload for the state being left (its caret now): it
    // rides to the opposite stack, so a redo hands back what the undo was
    // given (text_buffer::undo's `now`).
    bool doc_checkpoint(entity_id id, const madc::value &app_meta)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() || _publishing )
	    return false;
	text_buffer &b = _texts[id];
	b.checkpoint(d->second.checkpoint_meta(app_meta, b.version()));
	return true;
    }
    bool doc_undo(entity_id id, madc::value &app_out,
		  const madc::value &app_now = madc::value())
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	std::map<entity_id, text_buffer>::iterator t = _texts.find(id);
	app_out = madc::value();
	if ( d == _docs.end() || t == _texts.end() || !admit(id, false) )
	    return false;
	madc::value now = d->second.checkpoint_meta(app_now,
						    t->second.version());
	madc::value meta;
	size_t before = t->second.size();
	if ( !t->second.undo(meta, now) )
	    return false;
	d->second.restore_meta(meta, app_out, t->second.version());
	return whole_change(id, text_change::kind::undo, before);
    }
    bool doc_redo(entity_id id, madc::value &app_out,
		  const madc::value &app_now = madc::value())
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	std::map<entity_id, text_buffer>::iterator t = _texts.find(id);
	app_out = madc::value();
	if ( d == _docs.end() || t == _texts.end() || !admit(id, false) )
	    return false;
	madc::value now = d->second.checkpoint_meta(app_now,
						    t->second.version());
	madc::value meta;
	size_t before = t->second.size();
	if ( !t->second.redo(meta, now) )
	    return false;
	d->second.restore_meta(meta, app_out, t->second.version());
	return whole_change(id, text_change::kind::redo, before);
    }
    // --- anchors (design §2.3): the document's; 0 / false / absent when
    // the entity is no document.
    anchor_id anchor_add(entity_id id, size_t off, bool right)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	if ( d == _docs.end() )
	    return 0;
	std::map<entity_id, text_buffer>::const_iterator t = _texts.find(id);
	size_t size = t == _texts.end() ? 0 : t->second.size();
	return d->second.anchor_add(off > size ? size : off, right);
    }
    const anchor *anchor_of(entity_id id, anchor_id a) const
    {
	const document *d = doc_of(id);
	return d ? d->anchor_of(a) : (const anchor *)0;
    }
    bool anchor_move(entity_id id, anchor_id a, size_t off)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	return d != _docs.end() && d->second.anchor_move(a, off);
    }
    bool anchor_remove(entity_id id, anchor_id a)
    {
	std::map<entity_id, document>::iterator d = _docs.find(id);
	return d != _docs.end() && d->second.anchor_remove(a);
    }
    // The component, or NULL when the entity has none. (Reads only —
    // mutation goes through the verbs above.)
    const text_buffer *text_of(entity_id id) const
    {
	std::map<entity_id, text_buffer>::const_iterator it = _texts.find(id);
	return it == _texts.end() ? (const text_buffer *)0 : &it->second;
    }
    bool has_text(entity_id id) const { return _texts.count(id) != 0; }

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
