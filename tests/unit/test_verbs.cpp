// Unit battery for madcdis/verbs.h — Track 7 Phase 1 S2: the verb dispatch
// layer whose mutation_context is the only write surface (gate G4's
// mechanism), with requirement gating and data-carried refusal prose.
// Plan: docs/plans/2026-08-20-track7-phase1-text-adventure.md (S2).

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
using madc::hub::verb_table;
using madc::hub::verb_outcome;
using madc::hub::verb_status;

// A "take" shape: relink the target from wherever it is into the actor.
static bool take_handler(mutation_context &mc, entity_id actor,
			 entity_id target, const std::string &,
			 std::string &out)
{
    if ( target == 0 )
    {
	out = "take what?";
	return false;
    }
    name_id rel_in = mc.intern("in");
    entity_id holder = mc.view().target(target, rel_in, (name_id)0);
    if ( holder != 0 )
	mc.link_remove(target, rel_in, holder);
    mc.link_add(target, rel_in, actor);
    out = "taken";
    return true;
}

// A "light" shape: bag mutation through the context, with a data condition.
static bool light_handler(mutation_context &mc, entity_id,
			  entity_id target, const std::string &,
			  std::string &out)
{
    madc::hub::entity *e = mc.edit(target);
    if ( !e || !e->bag.is_object() )
    {
	out = "nothing to light";
	return false;
    }
    std::map<std::string, madc::value> &bag = e->bag.object();
    if ( bag.count("fuel") == 0 || bag["fuel"].as_integer() <= 0 )
    {
	out = "it is out of fuel";
	return false;
    }
    bag["lit"] = madc::value(true);
    out = "lit";
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
    verbs.register_verb(v_take, requirement(), std::string(), take_handler);
    CHECK(verbs.knows(v_take));
    CHECK(verbs.verb_count() == 1u);

    credentials none;
    verb_outcome r = verbs.invoke(w, none, v_take, hero, lamp, std::string());
    CHECK(r.ok());
    CHECK(r.status == verb_status::ok);
    CHECK(r.message == "taken");
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
			take_handler /* would relink if it ever ran */);

    credentials player;	// no builder key
    size_t links_before = w.sources(hero, w.intern("in")).size();
    verb_outcome r = verbs.invoke(w, player, v_edit, hero, wall, std::string());
    CHECK(r.status == verb_status::refused);
    CHECK(r.message == "Only a builder may reshape the world.");
    CHECK(w.sources(hero, w.intern("in")).size() == links_before);

    credentials builder;
    builder.grant_key(k_builder);
    verb_outcome ok = verbs.invoke(w, builder, v_edit, hero, wall, std::string());
    CHECK(ok.status == verb_status::ok);
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
			take_handler);

    credentials novice;
    novice.set_level(d_wizardry, 2);
    CHECK(verbs.invoke(w, novice, v_smite, 0, 0, std::string()).status
	  == verb_status::refused);

    credentials adept;
    adept.set_level(d_wizardry, 3);
    CHECK(verbs.invoke(w, adept, v_smite, 0, 0, std::string()).status
	  != verb_status::refused);

    credentials domainless;	// absent domain = no_level: refused
    CHECK(verbs.invoke(w, domainless, v_smite, 0, 0, std::string()).status
	  == verb_status::refused);
}

TEST_CASE("verb_table — unknown verb is a status, never prose; failed carries why")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id lamp = w.create("lamp");
    verb_table verbs;
    name_id v_light = w.intern("light");
    verbs.register_verb(v_light, requirement(), std::string(), light_handler);

    verb_outcome unknown = verbs.invoke(w, credentials(), w.intern("xyzzy"),
					hero, 0, std::string());
    CHECK(unknown.status == verb_status::unknown);
    CHECK(unknown.message.empty());	// the driver phrases it

    // Failed path: the lamp is out of fuel.
    madc::hub::entity *e = w.get(lamp);
    e->bag.object()["fuel"] = madc::value((int64_t)0);
    verb_outcome dry = verbs.invoke(w, credentials(), v_light, hero, lamp,
				    std::string());
    CHECK(dry.status == verb_status::failed);
    CHECK(dry.message == "it is out of fuel");
    CHECK(w.get(lamp)->bag.as_object().count("lit") == 0u);	// untouched

    e->bag.object()["fuel"] = madc::value((int64_t)10);
    verb_outcome lit = verbs.invoke(w, credentials(), v_light, hero, lamp,
				    std::string());
    CHECK(lit.status == verb_status::ok);
    CHECK(w.get(lamp)->bag.as_object().at("lit").as_boolean());
}
