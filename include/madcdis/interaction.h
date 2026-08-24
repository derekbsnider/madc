#ifndef __MADCDIS_INTERACTION_H
#define __MADCDIS_INTERACTION_H 1

// madcdis/interaction.h — the interaction-semantics data layer (Track 7.2
// R1): Context, Invocation, Availability, Affordance — what is relevant
// to an actor NOW, what the actor is attempting, and what the actor can
// presently do.
//
//   design: docs/plans/universal-application-interaction-rendering-abstraction.md
//   plan:   docs/plans/2026-08-24-ui-interaction-rework-and-texteditor.md
//
// THE SEAM LAW (design, Decisions Incorporated item 2): everything here is
// values and entity handles — the shapes an action binding of EITHER kind
// (native or script-entity) can accept and return. Nothing in this header
// may grow a member a madc script could not satisfy.
//
// This header is pure data + context building; the action registry, the
// binding contract, and affordance RESOLUTION live one layer up in
// madcdis/verbs.h (they consult the registry).
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): plain value
// objects and pure functions over the world they are handed; confined
// with it. Two copies never share state (value deep-copies).

#include <map>
#include <string>
#include <vector>

#include "madcdis/hub.h"

namespace madc {
namespace hub {

// --------------------------------------------------------- interaction_context
// The semantic scope relevant to an actor right now (design §2.7): who is
// acting, what holds their attention, which entities are reachable for
// target resolution, the interaction mode, and the task's own state
// (caret, selection, pending arguments — the interaction-state category,
// design §2.4). Ephemeral in Phase 1: built per interpretation, carried by
// the invocation it produced; a registered context identity arrives with
// multi-session serving.
struct interaction_context
{
    entity_id		    actor;
    entity_id		    focus;	// 0 = nowhere / nothing
    std::vector<entity_id>  scope;	// resolution order: carried, co-located, focus
    name_id		    mode;	// interned; 0 = the application's default
    madc::value		    interaction_state;	// object-kind bag; null until used

    interaction_context() : actor(0), focus(0), mode(0) {}
};

// Build the standard containment context: focus = where the actor is,
// scope = carried + co-located + the focus itself, in that resolution
// order. `held_rel` is the APPLICATION's containment relation — a
// parameter, like credentials_for's, because generic machinery carries no
// application vocabulary.
inline interaction_context containment_context(const world &w, entity_id actor,
					       name_id held_rel)
{
    interaction_context c;
    c.actor = actor;
    c.focus = w.target(actor, held_rel, (name_id)0);
    c.scope = w.sources(actor, held_rel);
    if ( c.focus != 0 )
    {
	std::vector<entity_id> here = w.sources(c.focus, held_rel);
	c.scope.insert(c.scope.end(), here.begin(), here.end());
	c.scope.push_back(c.focus);
    }
    return c;
}

// ----------------------------------------------------------------- invocation
// One concrete attempt: Actor -> Action(Target, Arguments) within a
// context (design §2.6). Arguments are named values — the frontend that
// built the invocation decided how they were collected; the binding that
// executes it never learns whether they came from a line, a key, a form,
// or an agent.
struct invocation
{
    entity_id	actor;
    name_id	action;
    entity_id	target;		// 0 = intransitive / unresolved
    std::map<name_id, madc::value> arguments;
    interaction_context context;	// the scope it was interpreted in

    invocation() : actor(0), action(0), target(0) {}

    const madc::value *argument(name_id key) const
    {
	std::map<name_id, madc::value>::const_iterator it = arguments.find(key);
	return it == arguments.end() ? (const madc::value *)0 : &it->second;
    }
    // String view of one argument; empty when absent or not string-kind.
    std::string text(name_id key) const
    {
	const madc::value *v = argument(key);
	return v && v->is_string() ? v->as_string() : std::string();
    }
};

// --------------------------------------------------------------- availability
// The truthful state of one action for one credential set (design §2.9):
// computed by the SAME keys+levels machinery that gates execution. A
// frontend may hide, disable, or explain from this — it never grants
// (design invariant 1); execution re-validates regardless (invariant 10).
struct availability
{
    bool	visible;
    bool	enabled;
    std::string	reason;		// refusal data when disabled; empty otherwise

    availability() : visible(true), enabled(true) {}
};

// ----------------------------------------------------------------- affordance
// An action currently available in a context (design §2.8): the action
// bound to a target, with the provider that made it available (an exit, a
// carried key, the application registry), any arguments the binding
// pre-supplies, a presentation label, and its truthful availability.
struct affordance
{
    name_id	action;
    entity_id	target;		// 0 = intransitive
    entity_id	provider;	// why this is available; 0 = the registry itself
    std::map<name_id, madc::value> bound_arguments;
    std::string	label;		// presentation label; empty = the action's spelling
    availability avail;

    affordance() : action(0), target(0), provider(0) {}
};

typedef std::vector<affordance> affordance_set;

} // namespace hub
} // namespace madc

#endif // __MADCDIS_INTERACTION_H
