#ifndef __MADCDIS_VERBS_H
#define __MADCDIS_VERBS_H 1

// madcdis/verbs.h — the hub verb layer (Track 7 Phase 1, slice S2).
//
//   design: docs/plans/2026-08-20-data-hub-projection-rendering.md
//   plan:   docs/plans/2026-08-20-track7-phase1-text-adventure.md
//
// EVERY MUTATION FLOWS THROUGH A VERB (design demand; pilot gate G4): the
// verb dispatcher hands its handler a mutation_context, and that context is
// the only write surface application layers ever receive. The context's
// methods mirror the world's mutators one-to-one today; the indirection is
// the SEAM — demand 15's diff journaling and change propagation attach here
// later without touching a handler.
//
// Phase 1 handlers are COMPILED functions bound by data (a registry keyed
// by interned verb name; requirements and refusal prose arrive from world
// data). Script-attached verb bodies are the eval/exec follow-up, behind
// this same interface.
//
// The generic layer carries NO application vocabulary and NO hardcoded
// prose: a refusal message is registered DATA; an unknown verb is a STATUS
// the driver phrases (prose is content, owned by the projection library).
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): confined to one
// thread, with the world it dispatches over. Verb invocation is the
// serializable mutation unit of design demand 15 — the shape a future
// multi-threaded hub serializes per world, changing no signatures.

#include <string>
#include <vector>

#include "madcdis/hub.h"

namespace madc {
namespace hub {

// ----------------------------------------------------------- mutation_context
// The one write surface. Reads go through view(); writes go through the
// mirrored mutators (never a raw world reference — the mirror is the
// journaling seam).
class mutation_context
{
    world &_w;

    mutation_context(const mutation_context &);
    mutation_context &operator=(const mutation_context &);
public:
    explicit mutation_context(world &w) : _w(w) {}

    const world &view() const { return _w; }
    name_id intern(const std::string &s) { return _w.intern(s); }

    entity_id create(const std::string &name) { return _w.create(name); }
    // Mutable entity access — bag writes happen through this, inside a
    // handler, nowhere else.
    entity *edit(entity_id id) { return _w.get(id); }
    void link_add(entity_id from, name_id rel, entity_id to, name_id key = 0)
    {
	_w.link_add(from, rel, to, key);
    }
    bool link_remove(entity_id from, name_id rel, entity_id to)
    {
	return _w.link_remove(from, rel, to);
    }
};

// ------------------------------------------------------------------ verb_table
// Dispatch outcome: a status the driver can phrase, plus a message that is
// either registered refusal DATA or handler-produced content.
enum class verb_status
{
    ok,		// handler ran and reported success
    unknown,	// no verb registered under that name
    refused,	// the verb's requirement was not satisfied
    failed	// handler ran and reported failure (message says why)
};

struct verb_outcome
{
    verb_status status;
    std::string message;
    verb_outcome() : status(verb_status::unknown) {}
    verb_outcome(verb_status s, const std::string &m) : status(s), message(m) {}
    bool ok() const { return status == verb_status::ok; }
};

// Handler contract: mutate ONLY through mc; return true for ok (out = the
// player-facing content the projection will carry), false for failed
// (out = why). `target` may be 0 for intransitive verbs; `arg` carries the
// raw word/rest-of-line the driver resolved nothing for.
typedef bool (*verb_handler)(mutation_context &mc, entity_id actor,
			     entity_id target, const std::string &arg,
			     std::string &out);

class verb_table
{
    struct verb_def
    {
	name_id	     name;
	requirement  req;
	std::string  refusal;	// registered data; empty = driver phrases it
	verb_handler fn;
	verb_def(name_id n, const requirement &r, const std::string &msg,
		 verb_handler f)
	    : name(n), req(r), refusal(msg), fn(f) {}
    };
    std::vector<verb_def> _verbs;	// linear; pilot scale

    const verb_def *find(name_id name) const
    {
	for ( size_t i = 0; i < _verbs.size(); ++i )
	    if ( _verbs[i].name == name )
		return &_verbs[i];
	return (const verb_def *)0;
    }
public:
    void register_verb(name_id name, const requirement &req,
		       const std::string &refusal, verb_handler fn)
    {
	_verbs.push_back(verb_def(name, req, refusal, fn));
    }
    bool knows(name_id name) const { return find(name) != (const verb_def *)0; }
    size_t verb_count() const { return _verbs.size(); }
    // The verb's own requirement (entity-attached conditions — a locked
    // door — are the HANDLER's to check against the same credentials).
    const requirement *requirement_of(name_id name) const
    {
	const verb_def *v = find(name);
	return v ? &v->req : (const requirement *)0;
    }

    verb_outcome invoke(world &w, const credentials &creds, name_id verb,
			entity_id actor, entity_id target,
			const std::string &arg) const
    {
	const verb_def *v = find(verb);
	if ( !v )
	    return verb_outcome(verb_status::unknown, std::string());
	if ( !v->req.satisfied_by(creds) )
	    return verb_outcome(verb_status::refused, v->refusal);
	mutation_context mc(w);
	std::string out;
	bool ok = v->fn(mc, actor, target, arg, out);
	return verb_outcome(ok ? verb_status::ok : verb_status::failed, out);
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_VERBS_H
