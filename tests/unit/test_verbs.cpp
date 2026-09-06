// Unit battery for madcdis/verbs.h — the action registry (Track 7 Phase 1
// S2; reworked to the interaction model in Track 7.2 R1): seam-law native
// bindings over a structured invocation, requirement gating through the
// same availability evaluator affordances read, data-carried refusal
// prose, and the script-entity binding kind behind an injected executor.
// Plan: docs/plans/2026-08-24-ui-interaction-rework-and-texteditor.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>

#include "madcdis/verbs.h"

using madc::hub::world;
using madc::hub::entity_id;
using madc::hub::name_id;
using madc::hub::credentials;
using madc::hub::requirement;
using madc::hub::mutation_context;
using madc::hub::action_env;
using madc::hub::invocation;
using madc::hub::availability;
using madc::hub::verb_table;
using madc::hub::verb_outcome;
using madc::hub::verb_status;

static invocation make_inv(const world &w, const char *verb, entity_id actor,
			   entity_id target, const std::string &arg)
{
    invocation inv;
    inv.actor = actor;
    inv.action = w.intern(verb);
    inv.target = target;
    inv.arguments[madc::hub::arg_key(w)] = madc::value(arg);
    return inv;
}

// A "take" shape: relink the target from wherever it is into the actor.
static bool take_binding(action_env &env, const invocation &inv,
			 madc::value &out)
{
    if ( inv.target == 0 )
    {
	out = madc::value("take what?");
	return false;
    }
    name_id rel_in = env.mc.intern("in");
    entity_id holder = env.mc.view().target(inv.target, rel_in, (name_id)0);
    if ( holder != 0 )
	env.mc.link_remove(inv.target, rel_in, holder);
    env.mc.link_add(inv.target, rel_in, inv.actor);
    out = madc::value("taken");
    return true;
}

// A "light" shape: bag mutation through the context, with a data condition.
static bool light_binding(action_env &env, const invocation &inv,
			  madc::value &out)
{
    madc::hub::entity *e = env.mc.edit(inv.target);
    if ( !e || !e->bag.is_object() )
    {
	out = madc::value("nothing to light");
	return false;
    }
    std::map<std::string, madc::value> &bag = e->bag.object();
    if ( bag.count("fuel") == 0 || bag["fuel"].as_integer() <= 0 )
    {
	out = madc::value("it is out of fuel");
	return false;
    }
    bag["lit"] = madc::value(true);
    out = madc::value("lit");
    return true;
}

TEST_CASE("verb_table — ok path mutates only through the context")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id room = w.create("room");
    entity_id lamp = w.create("lamp");
    name_id rel_in = w.intern("in");
    w.link_add(lamp, rel_in, room);
    w.link_add(hero, rel_in, room);

    verb_table verbs;
    name_id v_take = w.intern("take");
    verbs.register_verb(v_take, requirement(), std::string(), take_binding);
    CHECK(verbs.knows(v_take));
    CHECK(verbs.verb_count() == 1u);

    credentials none;
    verb_outcome r = verbs.invoke(w, none,
				  make_inv(w, "take", hero, lamp, ""));
    CHECK(r.ok());
    CHECK(r.status == verb_status::ok);
    CHECK(r.content.as_string() == "taken");
    CHECK(w.target(lamp, rel_in, (name_id)0) == hero);	// relinked
    CHECK(w.sources(room, rel_in).size() == 1u);	// only the hero remains
}

TEST_CASE("verb_table — refusal carries the registered data prose and mutates nothing")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id wall = w.create("wall");
    name_id v_edit = w.intern("edit");
    name_id k_builder = w.intern("builder");

    requirement builder_only;
    builder_only.keys.push_back(k_builder);

    verb_table verbs;
    verbs.register_verb(v_edit, builder_only,
			"Only a builder may reshape the world.",
			take_binding /* would relink if it ever ran */);

    credentials player;	// no builder key
    size_t links_before = w.sources(hero, w.intern("in")).size();
    verb_outcome r = verbs.invoke(w, player,
				  make_inv(w, "edit", hero, wall, ""));
    CHECK(r.status == verb_status::refused);
    CHECK(r.content.as_string() == "Only a builder may reshape the world.");
    CHECK(w.sources(hero, w.intern("in")).size() == links_before);

    credentials builder;
    builder.grant_key(k_builder);
    verb_outcome ok = verbs.invoke(w, builder,
				   make_inv(w, "edit", hero, wall, ""));
    CHECK(ok.status == verb_status::ok);
}

TEST_CASE("verb_table — availability is the refusal evaluator, surfaced")
{
    world w;
    name_id v_edit = w.intern("edit");
    name_id k_builder = w.intern("builder");

    requirement builder_only;
    builder_only.keys.push_back(k_builder);

    verb_table verbs;
    verbs.register_verb(v_edit, builder_only,
			"Only a builder may reshape the world.",
			take_binding);

    credentials player;
    availability a = verbs.availability_of(w, player,
					   make_inv(w, "edit", 0, 0, ""));
    CHECK(a.visible);
    CHECK(!a.enabled);
    CHECK(a.reason == "Only a builder may reshape the world.");

    credentials builder;
    builder.grant_key(k_builder);
    availability b = verbs.availability_of(w, builder,
					   make_inv(w, "edit", 0, 0, ""));
    CHECK(b.visible);
    CHECK(b.enabled);
    CHECK(b.reason.empty());

    availability none = verbs.availability_of(w, player,
					      make_inv(w, "xyzzy", 0, 0, ""));
    CHECK(!none.visible);
    CHECK(!none.enabled);
}

TEST_CASE("verb_table — leveled verb refuses below the domain level")
{
    world w;
    name_id v_smite = w.intern("smite");
    name_id d_wizardry = w.intern("wizardry");

    requirement needs_wiz3;
    needs_wiz3.level_domain = d_wizardry;
    needs_wiz3.min_level = 3;

    verb_table verbs;
    verbs.register_verb(v_smite, needs_wiz3, "You lack the wizardry.",
			take_binding);

    credentials novice;
    novice.set_level(d_wizardry, 2);
    CHECK(verbs.invoke(w, novice, make_inv(w, "smite", 0, 0, "")).status
	  == verb_status::refused);

    credentials adept;
    adept.set_level(d_wizardry, 3);
    CHECK(verbs.invoke(w, adept, make_inv(w, "smite", 0, 0, "")).status
	  != verb_status::refused);

    credentials domainless;	// absent domain = no_level: refused
    CHECK(verbs.invoke(w, domainless, make_inv(w, "smite", 0, 0, "")).status
	  == verb_status::refused);
}

TEST_CASE("verb_table — unknown verb is a status, never prose; failed carries why")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id lamp = w.create("lamp");
    verb_table verbs;
    name_id v_light = w.intern("light");
    verbs.register_verb(v_light, requirement(), std::string(), light_binding);

    verb_outcome unknown = verbs.invoke(w, credentials(),
					make_inv(w, "xyzzy", hero, 0, ""));
    CHECK(unknown.status == verb_status::unknown);
    CHECK(unknown.content.is_null());	// the driver phrases it

    // Failed path: the lamp is out of fuel.
    madc::hub::entity *e = w.get(lamp);
    e->bag.object()["fuel"] = madc::value((int64_t)0);
    verb_outcome dry = verbs.invoke(w, credentials(),
				    make_inv(w, "light", hero, lamp, ""));
    CHECK(dry.status == verb_status::failed);
    CHECK(dry.content.as_string() == "it is out of fuel");
    CHECK(w.get(lamp)->bag.as_object().count("lit") == 0u);	// untouched

    e->bag.object()["fuel"] = madc::value((int64_t)10);
    verb_outcome lit = verbs.invoke(w, credentials(),
				    make_inv(w, "light", hero, lamp, ""));
    CHECK(lit.status == verb_status::ok);
    CHECK(w.get(lamp)->bag.as_object().at("lit").as_boolean());
}

// The script-entity binding kind: the registry stores SOURCE; execution
// is delegated to the injected executor — same env, same invocation, same
// gating, same outcome shape as the native kind.
static std::string exec_seen_source;
static bool fake_executor(action_env &env, const invocation &inv,
			  const std::string &source, madc::value &out)
{
    exec_seen_source = source;
    // Prove env + invocation reach the executor: mutate through the one
    // write surface and answer with the argument text.
    env.mc.edit(inv.actor)->bag.object()["ran"] = madc::value(true);
    out = madc::value("script:" + inv.text(madc::hub::arg_key(env.mc.view())));
    return true;
}

TEST_CASE("verb_table — script kind dispatches through the injected executor")
{
    world w;
    entity_id hero = w.create("hero");
    verb_table verbs;
    name_id v_wave = w.intern("wave");
    verbs.register_script_verb(v_wave, requirement(), std::string(),
			       "return \"hi\";");
    CHECK(verbs.knows(v_wave));

    // No executor injected: fails loudly, never silently succeeds.
    verb_outcome dark = verbs.invoke(w, credentials(),
				     make_inv(w, "wave", hero, 0, "x"));
    CHECK(dark.status == verb_status::failed);

    verbs.set_script_executor(fake_executor);
    exec_seen_source.clear();
    verb_outcome r = verbs.invoke(w, credentials(),
				  make_inv(w, "wave", hero, 0, "hello"), 42);
    CHECK(r.status == verb_status::ok);
    CHECK(r.content.as_string() == "script:hello");
    CHECK(exec_seen_source == "return \"hi\";");
    CHECK(w.get(hero)->bag.as_object().at("ran").as_boolean());

    // Gating applies identically to the script kind.
    requirement builder_only;
    builder_only.keys.push_back(w.intern("builder"));
    verbs.register_script_verb(w.intern("shape"), builder_only, "No.",
			       "return \"never\";");
    verb_outcome refused = verbs.invoke(w, credentials(),
					make_inv(w, "shape", hero, 0, ""));
    CHECK(refused.status == verb_status::refused);
    CHECK(refused.content.as_string() == "No.");
}

// State-conditional availability (the check binding, both kinds): the
// SAME evaluator answers enumeration and dispatch, so they can never
// disagree (design invariant 5).
static bool locked_when_flagged(action_env &env, const invocation &inv,
				std::string &reason)
{
    const madc::hub::entity *e = env.mc.view().get(inv.actor);
    if ( e && e->bag.is_object() && e->bag.as_object().count("frozen") )
    {
	reason = "You are frozen solid.";
	return false;
    }
    return true;
}

TEST_CASE("verb_table — native check gates enumeration and dispatch identically")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id lamp = w.create("lamp");
    verb_table verbs;
    name_id v_take = w.intern("take");
    verbs.register_verb(v_take, requirement(), std::string(), take_binding);
    CHECK(verbs.set_check(v_take, locked_when_flagged));
    CHECK_FALSE(verbs.set_check(w.intern("xyzzy"), locked_when_flagged));

    credentials none;
    invocation inv = make_inv(w, "take", hero, lamp, "");
    availability a = verbs.availability_of(w, none, inv);
    CHECK(a.enabled);
    CHECK(verbs.invoke(w, none, inv).status == verb_status::ok);

    w.get(hero)->bag.object()["frozen"] = madc::value(true);
    availability frozen = verbs.availability_of(w, none, inv);
    CHECK(frozen.visible);
    CHECK(!frozen.enabled);
    CHECK(frozen.reason == "You are frozen solid.");
    verb_outcome r = verbs.invoke(w, none, inv);
    CHECK(r.status == verb_status::refused);
    CHECK(r.content.as_string() == "You are frozen solid.");

    w.get(hero)->bag.object().erase("frozen");
    CHECK(verbs.availability_of(w, none, inv).enabled);
}

TEST_CASE("verb_table — requirement refusal outranks the check; script checks answer ok/reason")
{
    world w;
    entity_id hero = w.create("hero");
    verb_table verbs;
    name_id v_shape = w.intern("shape");
    requirement builder_only;
    builder_only.keys.push_back(w.intern("builder"));
    verbs.register_verb(v_shape, builder_only, "Builders only.", take_binding);
    CHECK(verbs.set_script_check(v_shape, "CHECK_NO"));
    CHECK_FALSE(verbs.set_script_check(w.intern("xyzzy"), "CHECK_NO"));

    // No executor injected: the check cannot silently pass.
    credentials builder;
    builder.grant_key(w.intern("builder"));
    invocation inv = make_inv(w, "shape", hero, 0, "");
    availability dark = verbs.availability_of(w, builder, inv);
    CHECK(!dark.enabled);
    CHECK(dark.reason == "script check has no executor");

    // The executor answers by protocol: "ok" = available, text = the
    // disabled reason, empty (an eval failure's shape) = loud generic.
    verbs.set_script_executor(
	[](action_env &, const invocation &, const std::string &source,
	   madc::value &out) -> bool
	{
	    if ( source == "CHECK_OK" )
		out = madc::value(std::string("ok"));
	    else if ( source == "CHECK_NO" )
		out = madc::value(std::string("The clay is fired."));
	    return true;	// CHECK_EMPTY: out stays null
	});

    // The unmet requirement answers FIRST — its refusal, not the check's.
    credentials player;
    availability req = verbs.availability_of(w, player, inv);
    CHECK(!req.enabled);
    CHECK(req.reason == "Builders only.");

    availability no = verbs.availability_of(w, builder, inv);
    CHECK(!no.enabled);
    CHECK(no.reason == "The clay is fired.");
    CHECK(verbs.invoke(w, builder, inv).status == verb_status::refused);

    verbs.set_script_check(v_shape, "CHECK_OK");
    CHECK(verbs.availability_of(w, builder, inv).enabled);
    CHECK(verbs.invoke(w, builder, inv).status != verb_status::refused);

    verbs.set_script_check(v_shape, "CHECK_EMPTY");
    availability broken = verbs.availability_of(w, builder, inv);
    CHECK(!broken.enabled);
    CHECK(broken.reason == "availability check failed");
}

TEST_CASE("mutation_context — text-component writes mirror through the one surface")
{
    world w;
    entity_id doc = w.create("doc");
    mutation_context mc(w);
    CHECK_FALSE(mc.view().has_text(doc));

    mc.text_load(doc, "one\ntwo\n");
    REQUIRE(mc.view().has_text(doc));
    CHECK(mc.view().text_of(doc)->line_count() == 2u);

    mc.text_insert(doc, 0, "zero\n");
    mc.text_erase(doc, 5, 4);		// "one\n" goes
    mc.text_replace(doc, 5, 3, "TWO");
    CHECK(mc.view().text_of(doc)->text() == "zero\nTWO\n");

    // A bare entity has no component; reads answer absence, not a crash.
    entity_id bare = w.create("bare");
    CHECK(mc.view().text_of(bare) == (const madc::hub::text_buffer *)0);
}

// Re-entrancy is ENFORCED (the R3 sibling design): a binding that
// re-enters the registry receives a refusal as ITS nested result; the
// outer invocation completes normally, and the latch clears after it.
static verb_table *g_reenter_table = (verb_table *)0;
static world *g_reenter_world = (world *)0;
static bool reenter_binding(action_env &env, const invocation &inv,
			    madc::value &out)
{
    invocation nested;
    nested.actor = inv.actor;
    nested.action = env.mc.intern("inner");
    verb_outcome n = g_reenter_table->invoke(*g_reenter_world, env.creds,
					     nested, env.session);
    out = n.content;
    return n.status == verb_status::refused;
}
static bool inner_binding(action_env &, const invocation &, madc::value &out)
{
    out = madc::value(std::string("inner ran"));
    return true;
}

TEST_CASE("verb_table — re-entering the registry is refused; the latch clears")
{
    world w;
    entity_id hero = w.create("hero");
    verb_table verbs;
    g_reenter_table = &verbs;
    g_reenter_world = &w;
    verbs.register_verb(w.intern("outer"), requirement(), std::string(),
			reenter_binding);
    verbs.register_verb(w.intern("inner"), requirement(), std::string(),
			inner_binding);
    credentials none;
    verb_outcome r = verbs.invoke(w, none, make_inv(w, "outer", hero, 0, ""));
    CHECK(r.ok());	// the outer binding SAW the refusal and reported it
    CHECK(r.content.as_string()
	  == "action re-entered the registry (verbs do not re-enter act)");
    // The latch cleared: the next top-level invoke is ordinary.
    r = verbs.invoke(w, none, make_inv(w, "inner", hero, 0, ""));
    CHECK(r.ok());
    CHECK(r.content.as_string() == "inner ran");
    g_reenter_table = (verb_table *)0;
    g_reenter_world = (world *)0;
}
