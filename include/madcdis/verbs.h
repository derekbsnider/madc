#ifndef __MADCDIS_VERBS_H
#define __MADCDIS_VERBS_H 1

// madcdis/verbs.h — the action registry and binding layer (Track 7 Phase 1
// S2; reworked to the interaction model in Track 7.2 R1).
//
//   design: docs/plans/universal-application-interaction-rendering-abstraction.md
//   plan:   docs/plans/2026-08-24-ui-interaction-rework-and-texteditor.md
//
// EVERY MUTATION FLOWS THROUGH A VERB (design demand; pilot gate G4): the
// dispatcher hands its binding a mutation_context, and that context is the
// only write surface application layers ever receive. The context's
// methods mirror the world's mutators one-to-one today; the indirection is
// the SEAM — demand 15's diff journaling and change propagation attach here
// later without touching a binding.
//
// ONE REGISTRY, TWO BINDING KINDS (design, Decisions Incorporated item 1):
// this registry is where BOTH kinds live. The native kind is a compiled
// host function (a libmadc-embedding host's kind); the script-entity kind
// is a verb whose body is madc SOURCE, run through the injected script
// executor. Same registration, same invocation, same gating, same
// outcome. The engine itself ships ZERO verbs of either kind —
// applications supply them (Rule #7: no application specifics in general
// machinery).
//
// THE SEAM LAW (item 2, gating every binding signature): a binding takes
// the execution environment (reachable through a session/world HANDLE on
// the script side) plus a structured invocation of values and entity
// handles, and returns a value-shaped result. No binding signature may
// accept or return anything a madc script could not.
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
#include "madcdis/interaction.h"

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
    // binding, nowhere else.
    entity *edit(entity_id id) { return _w.get(id); }
    void link_add(entity_id from, name_id rel, entity_id to, name_id key = 0)
    {
	_w.link_add(from, rel, to, key);
    }
    bool link_remove(entity_id from, name_id rel, entity_id to)
    {
	return _w.link_remove(from, rel, to);
    }
    // Text-component writes (R4): the same one-write-surface mirror.
    void text_load(entity_id id, const std::string &s)
    {
	_w.text_load(id, s);
    }
    void text_insert(entity_id id, size_t off, const std::string &s)
    {
	_w.text_insert(id, off, s);
    }
    void text_erase(entity_id id, size_t off, size_t len)
    {
	_w.text_erase(id, off, len);
    }
    void text_replace(entity_id id, size_t off, size_t len,
		      const std::string &s)
    {
	_w.text_replace(id, off, len, s);
    }
};

// ------------------------------------------------------------------ action_env
// The environment a binding EXECUTES WITHIN: the mutation surface, the
// invoker's credentials (entity-attached conditions — a locked door's data
// requirement — are the binding's to check against them; the ACTION's own
// requirement is already checked by dispatch), and the host's opaque
// session handle (0 when the host has none). On the script side this
// whole environment is reachable through that handle; the invocation is
// the DATA; the result is value-shaped. (The seam law.)
struct action_env
{
    mutation_context  &mc;
    const credentials &creds;
    int64_t	       session;

    action_env(mutation_context &m, const credentials &c, int64_t s)
	: mc(m), creds(c), session(s) {}
};

// The pre-schema argument convention: interpretation binds the unresolved
// rest of the input line as the ONE argument, interned "arg". Parameter
// schemas (design §2.5) supersede this when they land.
inline name_id arg_key(const world &w) { return w.intern("arg"); }

// Native binding kind: execute the invocation, mutating ONLY through
// env.mc; return true for ok with `out` = player-facing content, false
// for failed with `out` = why. Both arms are value-shaped.
typedef bool (*action_binding)(action_env &env, const invocation &inv,
			       madc::value &out);

// Script-entity binding kind: the verb's body is stored madc SOURCE — a
// code entity — and execution is delegated to the registered script
// executor, the eval seam injected ONCE by the layer that owns eval
// (never a header dependency on the eval engine). Same env, same
// invocation, same value-shaped result as the native kind.
typedef bool (*script_executor)(action_env &env, const invocation &inv,
				const std::string &source, madc::value &out);

// ------------------------------------------------------------------ verb_table
// Dispatch outcome: a status the driver can phrase, plus value-shaped
// content that is either registered refusal DATA or binding-produced.
enum class verb_status
{
    ok,		// binding ran and reported success
    unknown,	// no verb registered under that name
    refused,	// the verb's availability said no (requirement unmet)
    failed	// binding ran and reported failure (content says why)
};

struct verb_outcome
{
    verb_status status;
    madc::value content;
    verb_outcome() : status(verb_status::unknown) {}
    verb_outcome(verb_status s, const madc::value &c) : status(s), content(c) {}
    bool ok() const { return status == verb_status::ok; }
};

class verb_table
{
    struct verb_def
    {
	name_id	       name;
	requirement    req;
	std::string    refusal;	// registered data; empty = driver phrases it
	action_binding fn;	// native kind; null for the script kind
	std::string    source;	// script kind; empty for the native kind
	verb_def(name_id n, const requirement &r, const std::string &msg,
		 action_binding f, const std::string &src)
	    : name(n), req(r), refusal(msg), fn(f), source(src) {}
    };
    std::vector<verb_def> _verbs;	// linear; pilot scale
    script_executor _exec;		// the injected eval seam; may be null

    const verb_def *find(name_id name) const
    {
	for ( size_t i = 0; i < _verbs.size(); ++i )
	    if ( _verbs[i].name == name )
		return &_verbs[i];
	return (const verb_def *)0;
    }
public:
    verb_table() : _exec((script_executor)0) {}

    void set_script_executor(script_executor exec) { _exec = exec; }

    void register_verb(name_id name, const requirement &req,
		       const std::string &refusal, action_binding fn)
    {
	_verbs.push_back(verb_def(name, req, refusal, fn, std::string()));
    }
    void register_script_verb(name_id name, const requirement &req,
			      const std::string &refusal,
			      const std::string &source)
    {
	_verbs.push_back(verb_def(name, req, refusal, (action_binding)0,
				  source));
    }
    bool knows(name_id name) const { return find(name) != (const verb_def *)0; }
    size_t verb_count() const { return _verbs.size(); }
    // Registered names in registration order — the affordance resolver's
    // application-action enumeration.
    std::vector<name_id> verb_names() const
    {
	std::vector<name_id> out;
	for ( size_t i = 0; i < _verbs.size(); ++i )
	    out.push_back(_verbs[i].name);
	return out;
    }
    // The verb's own requirement (entity-attached conditions — a locked
    // door — are the BINDING's to check against the same credentials).
    const requirement *requirement_of(name_id name) const
    {
	const verb_def *v = find(name);
	return v ? &v->req : (const requirement *)0;
    }

    // The truthful availability of one action for one credential set —
    // the SAME keys+levels evaluation that gates execution below. Phase 1
    // policy: an unmet requirement is visible-but-disabled with the
    // refusal data as the reason (the shipped refusal behavior already
    // names the verb; hiding is a frontend choice made ON this state).
    // An unregistered name is simply not there.
    availability availability_of(name_id name, const credentials &creds) const
    {
	availability a;
	const verb_def *v = find(name);
	if ( !v )
	{
	    a.visible = false;
	    a.enabled = false;
	    return a;
	}
	if ( !v->req.satisfied_by(creds) )
	{
	    a.enabled = false;
	    a.reason = v->refusal;
	}
	return a;
    }

    // Validate and execute one invocation (design §5: the invocation is
    // re-validated here no matter what any frontend advertised). `session`
    // is the host's opaque handle, passed through to the binding's
    // environment. A script verb with no injected executor fails loudly in
    // the outcome — never silently succeeds.
    verb_outcome invoke(world &w, const credentials &creds,
			const invocation &inv, int64_t session = 0) const
    {
	const verb_def *v = find(inv.action);
	if ( !v )
	    return verb_outcome(verb_status::unknown, madc::value());
	availability a = availability_of(inv.action, creds);
	if ( !a.enabled )
	    return verb_outcome(verb_status::refused, madc::value(a.reason));
	mutation_context mc(w);
	action_env env(mc, creds, session);
	madc::value out;
	bool ok;
	if ( v->fn )
	    ok = v->fn(env, inv, out);
	else if ( _exec )
	    ok = _exec(env, inv, v->source, out);
	else
	{
	    out = madc::value("script verb has no executor");
	    ok = false;
	}
	return verb_outcome(ok ? verb_status::ok : verb_status::failed, out);
    }
};

// -------------------------------------------------------- affordance resolving
// A gatherer contributes context-bound affordances (an exit's "go north",
// a portable item's "take key"): application code that knows its own
// vocabulary, run by the generic resolver. Availability on every emitted
// affordance is the emitter's duty — the resolver only assembles.
typedef void (*affordance_gatherer)(const world &w, const verb_table &verbs,
				    const credentials &creds,
				    const interaction_context &ctx,
				    affordance_set &out);

// resolve_affordances (design §2.8): application actions + context-bound
// contributions − prohibitions. Every entry carries truthful availability
// from the same evaluator that gates execution.
inline affordance_set resolve_affordances(
    const world &w, const verb_table &verbs, const credentials &creds,
    const interaction_context &ctx,
    const std::vector<affordance_gatherer> &gatherers)
{
    affordance_set out;
    // Application actions: every registered verb, in registration order.
    std::vector<name_id> names = verbs.verb_names();
    for ( size_t i = 0; i < names.size(); ++i )
    {
	affordance a;
	a.action = names[i];
	a.avail = verbs.availability_of(names[i], creds);
	out.push_back(a);
    }
    // Context-bound enrichment: actor / focus / related-resource actions,
    // contributed by the application's gatherers.
    for ( size_t i = 0; i < gatherers.size(); ++i )
	gatherers[i](w, verbs, creds, ctx, out);
    // − prohibited(mode): no prohibition machinery exists yet — this is
    // its seat; mode-keyed prohibitions arrive as data when modes do.
    return out;
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_VERBS_H
