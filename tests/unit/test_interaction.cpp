// Unit battery for madcdis/interaction.h + the affordance resolver in
// madcdis/verbs.h (Track 7.2 R1): explicit interaction contexts,
// structured invocations, and resolve_affordances — registry actions with
// truthful availability plus application-gathered, context-bound entries.
// Plan: docs/plans/2026-08-24-ui-interaction-rework-and-texteditor.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madcdis/verbs.h"

using madc::hub::world;
using madc::hub::entity_id;
using madc::hub::name_id;
using madc::hub::credentials;
using madc::hub::requirement;
using madc::hub::action_env;
using madc::hub::interaction_context;
using madc::hub::invocation;
using madc::hub::availability;
using madc::hub::affordance;
using madc::hub::affordance_set;
using madc::hub::affordance_gatherer;
using madc::hub::verb_table;

static bool noop_binding(action_env &, const invocation &, madc::value &out)
{
    out = madc::value("ok");
    return true;
}

TEST_CASE("containment_context — focus and resolution-ordered scope")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id room = w.create("room");
    entity_id sack = w.create("sack");
    entity_id rock = w.create("rock");
    name_id rel_in = w.intern("in");
    w.link_add(hero, rel_in, room);
    w.link_add(sack, rel_in, hero);	// carried
    w.link_add(rock, rel_in, room);	// co-located

    interaction_context ctx =
	madc::hub::containment_context(w, hero, rel_in);
    CHECK(ctx.actor == hero);
    CHECK(ctx.focus == room);
    // Order: carried, then co-located (hero included — it is in the room),
    // then the focus itself.
    REQUIRE(ctx.scope.size() == 4u);
    CHECK(ctx.scope[0] == sack);
    CHECK(ctx.scope[3] == room);
    CHECK(ctx.mode == 0);
    CHECK(ctx.interaction_state.is_null());

    // No containment: no focus, empty scope.
    entity_id ghost = w.create("ghost");
    interaction_context lost =
	madc::hub::containment_context(w, ghost, rel_in);
    CHECK(lost.focus == 0);
    CHECK(lost.scope.empty());
}

TEST_CASE("invocation — named value arguments and the text view")
{
    world w;
    invocation inv;
    name_id k = madc::hub::arg_key(w);
    CHECK(inv.argument(k) == (const madc::value *)0);
    CHECK(inv.text(k).empty());

    inv.arguments[k] = madc::value(std::string("north"));
    REQUIRE(inv.argument(k) != (const madc::value *)0);
    CHECK(inv.text(k) == "north");

    // Non-string arguments are values, not coerced text.
    name_id kn = w.intern("count");
    inv.arguments[kn] = madc::value((int64_t)3);
    CHECK(inv.text(kn).empty());
    CHECK(inv.argument(kn)->as_integer() == 3);
}

// An application gatherer: contribute one bound affordance per `exit`
// link of the focus (application vocabulary lives HERE, never in the
// resolver).
static void exit_gatherer(const world &w, const verb_table &verbs,
			  const credentials &creds,
			  const interaction_context &ctx, affordance_set &out)
{
    name_id v_go = w.intern("go");
    if ( !verbs.knows(v_go) || ctx.focus == 0 )
	return;
    std::vector<name_id> dirs = w.link_keys(ctx.focus, w.intern("exit"));
    for ( size_t i = 0; i < dirs.size(); ++i )
    {
	affordance a;
	a.action = v_go;
	a.target = w.target(ctx.focus, w.intern("exit"), dirs[i]);
	a.provider = ctx.focus;
	a.bound_arguments[madc::hub::arg_key(w)]
	    = madc::value(std::string(w.spelling(dirs[i])));
	a.label = std::string("go ") + w.spelling(dirs[i]);
	a.avail = verbs.availability_of(v_go, creds);
	out.push_back(a);
    }
}

TEST_CASE("resolve_affordances — registry truth plus gathered context entries")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id room = w.create("room");
    entity_id hall = w.create("hall");
    name_id rel_in = w.intern("in");
    w.link_add(hero, rel_in, room);
    w.link_add(room, w.intern("exit"), hall, w.intern("north"));

    verb_table verbs;
    verbs.register_verb(w.intern("go"), requirement(), std::string(),
			noop_binding);
    requirement builder_only;
    builder_only.keys.push_back(w.intern("builder"));
    verbs.register_verb(w.intern("edit"), builder_only, "Builders only.",
			noop_binding);

    interaction_context ctx =
	madc::hub::containment_context(w, hero, rel_in);
    std::vector<affordance_gatherer> gatherers;
    gatherers.push_back(exit_gatherer);

    credentials player;
    affordance_set set = madc::hub::resolve_affordances(w, verbs, player,
							ctx, gatherers);
    // Registry entries in registration order, then the gathered one.
    REQUIRE(set.size() == 3u);
    CHECK(set[0].action == w.intern("go"));
    CHECK(set[0].target == 0);
    CHECK(set[0].avail.enabled);
    CHECK(set[1].action == w.intern("edit"));
    CHECK(set[1].avail.visible);
    CHECK(!set[1].avail.enabled);
    CHECK(set[1].avail.reason == "Builders only.");
    CHECK(set[2].action == w.intern("go"));
    CHECK(set[2].target == hall);
    CHECK(set[2].provider == room);
    CHECK(set[2].label == "go north");
    REQUIRE(set[2].bound_arguments.count(madc::hub::arg_key(w)) == 1u);
    CHECK(set[2].bound_arguments.find(madc::hub::arg_key(w))
	      ->second.as_string() == "north");

    // The same enumeration with the key granted flips edit to enabled —
    // the SAME evaluator execution gating reads.
    credentials builder;
    builder.grant_key(w.intern("builder"));
    affordance_set bset = madc::hub::resolve_affordances(w, verbs, builder,
							 ctx, gatherers);
    CHECK(bset[1].avail.enabled);
    CHECK(bset[1].avail.reason.empty());
}
