// Unit battery for madcdis/hub.h — the Track 7 Phase 1 S1 substrate:
// entities + value-bag component + keyed links, and the owner's access
// model (keys with layered implication closure, per-domain levels,
// requirement checks, containment-derived credentials).
// Plan: docs/plans/2026-08-20-track7-phase1-text-adventure.md (S1).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madcdis/hub.h"

using madc::hub::world;
using madc::hub::entity;
using madc::hub::entity_id;
using madc::hub::name_id;
using madc::hub::credentials;
using madc::hub::requirement;

TEST_CASE("hub::world — entities: create, get, find, bag round-trip")
{
    world w;
    entity_id lamp = w.create("brass-lantern");
    entity_id room = w.create("twisty-passage");
    CHECK(lamp != 0u);
    CHECK(room != 0u);
    CHECK(lamp != room);
    CHECK(w.entity_count() == 2u);

    entity *e = w.get(lamp);
    REQUIRE(e != (entity *)0);
    CHECK(std::string(w.spelling(e->name)) == "brass-lantern");

    // The Phase-1 component: an object-kind value bag, vivified on first use.
    e->bag.object()["fuel"] = madc::value((int64_t)50);
    e->bag.object()["lit"] = madc::value(false);
    e->bag.object()["desc"] = madc::value("A dented brass lantern.");
    CHECK(w.get(lamp)->bag.as_object().at("fuel").as_integer() == 50);
    CHECK(w.get(lamp)->bag.as_object().at("lit").as_boolean() == false);
    CHECK(w.get(lamp)->bag.as_object().at("desc").as_string()
	  == "A dented brass lantern.");

    CHECK(w.find("twisty-passage") == room);
    CHECK(w.find("no-such-thing") == 0u);
    CHECK(w.get((entity_id)0) == (entity *)0);
    CHECK(w.get((entity_id)999) == (entity *)0);
}

TEST_CASE("hub::world — keyed links: exits, containment, remove, reverse")
{
    world w;
    entity_id r1 = w.create("room-1");
    entity_id r2 = w.create("room-2");
    entity_id sack = w.create("sack");
    entity_id key = w.create("brass-key");
    entity_id hero = w.create("hero");

    name_id rel_exit = w.intern("exit");
    name_id rel_in = w.intern("in");
    name_id north = w.intern("north");
    name_id east = w.intern("east");

    // Geography: keyed single-target links.
    w.link_add(r1, rel_exit, r2, north);
    w.link_add(r2, rel_exit, r1, east);
    CHECK(w.target(r1, rel_exit, north) == r2);
    CHECK(w.target(r2, rel_exit, east) == r1);
    CHECK(w.target(r1, rel_exit, east) == 0u);	// no such exit

    std::vector<name_id> exits = w.link_keys(r1, rel_exit);
    REQUIRE(exits.size() == 1u);
    CHECK(std::string(w.spelling(exits[0])) == "north");

    // Containment: unkeyed links; contents read through the reverse index.
    w.link_add(sack, rel_in, hero);	// hero carries the sack
    w.link_add(key, rel_in, sack);	// the key is inside the sack
    std::vector<entity_id> carried = w.sources(hero, rel_in);
    REQUIRE(carried.size() == 1u);
    CHECK(carried[0] == sack);
    CHECK(w.target(sack, rel_in, (name_id)0) == hero);	// where is the sack?

    // take/drop = relink: remove one edge, add another.
    CHECK(w.link_remove(key, rel_in, sack));
    w.link_add(key, rel_in, hero);
    CHECK(w.sources(sack, rel_in).empty());
    CHECK(w.sources(hero, rel_in).size() == 2u);
    CHECK_FALSE(w.link_remove(key, rel_in, sack));	// already gone
}

TEST_CASE("hub::credentials — keys, levels, and the requirement matrix")
{
    world w;
    name_id builder = w.intern("builder");
    name_id player = w.intern("player");
    name_id wizardry = w.intern("wizardry");

    credentials c;
    CHECK_FALSE(c.has_key(builder));
    c.grant_key(builder);
    CHECK(c.has_key(builder));
    CHECK_FALSE(c.has_key(player));
    CHECK(c.key_count() == 1u);

    // Absent domain reads the identity that fails every legal gate.
    CHECK(c.level(wizardry) == credentials::no_level);
    c.set_level(wizardry, 3);
    CHECK(c.level(wizardry) == 3);

    requirement none;
    CHECK(none.empty());
    CHECK(none.satisfied_by(c));	// no condition = open

    requirement needs_builder;
    needs_builder.keys.push_back(builder);
    CHECK(needs_builder.satisfied_by(c));

    requirement needs_both;
    needs_both.keys.push_back(builder);
    needs_both.keys.push_back(player);
    CHECK_FALSE(needs_both.satisfied_by(c));	// AND semantics

    requirement needs_level;
    needs_level.level_domain = wizardry;
    needs_level.min_level = 3;
    CHECK(needs_level.satisfied_by(c));		// 3 >= 3
    needs_level.min_level = 4;
    CHECK_FALSE(needs_level.satisfied_by(c));	// 3 < 4

    requirement combined;			// keys AND level together
    combined.keys.push_back(builder);
    combined.level_domain = wizardry;
    combined.min_level = 1;
    CHECK(combined.satisfied_by(c));

    credentials leveless;
    leveless.grant_key(builder);
    CHECK_FALSE(needs_level.satisfied_by(leveless));	// absent domain fails
}

TEST_CASE("hub::world — key implication closure is transitive and cycle-safe")
{
    world w;
    name_id master = w.intern("area-master");
    name_id doors = w.intern("area-doors");
    name_id chests = w.intern("area-chests");
    name_id inner = w.intern("inner-vault");

    w.key_implies(master, doors);
    w.key_implies(master, chests);
    w.key_implies(chests, inner);	// layered: master -> chests -> inner
    w.key_implies(inner, master);	// a CYCLE back to the top

    credentials c;
    c.grant_key(master);
    w.close_over_implications(c);
    CHECK(c.has_key(master));
    CHECK(c.has_key(doors));
    CHECK(c.has_key(chests));
    CHECK(c.has_key(inner));
    CHECK(c.key_count() == 4u);

    // Holding only a LEAF grants nothing extra (implication is directed) —
    // except through the cycle, which climbs back up by design here.
    credentials d;
    d.grant_key(doors);
    w.close_over_implications(d);
    CHECK(d.key_count() == 1u);
}

TEST_CASE("hub::world — containment-derived credentials (the brass key)")
{
    world w;
    entity_id hero = w.create("hero");
    entity_id sack = w.create("sack");
    entity_id brass = w.create("brass-key");
    entity_id decoy = w.create("decoy-rock");
    entity_id floor_key = w.create("floor-key");

    name_id rel_in = w.intern("in");
    name_id opens_brass = w.intern("opens-brass-door");

    // The key entity CONFERS a capability through its bag: data-derived
    // credentials, the design's brass-key case — transitively, inside a
    // carried sack.
    w.get(brass)->bag.object()["grants"] =
	madc::value("opens-brass-door");
    w.get(floor_key)->bag.object()["grants"] =
	madc::value("opens-brass-door");
    w.link_add(sack, rel_in, hero);
    w.link_add(brass, rel_in, sack);
    w.link_add(decoy, rel_in, hero);	// bag-less carried entity: ignored
    // floor_key lies elsewhere: NOT carried, must not confer.

    credentials session;	// no session grants at all
    credentials c = w.credentials_for(hero, session, rel_in, "grants");
    CHECK(c.has_key(opens_brass));

    requirement door;
    door.keys.push_back(opens_brass);
    CHECK(door.satisfied_by(c));

    // Drop the sack: the derived key goes with it.
    CHECK(w.link_remove(sack, rel_in, hero));
    credentials after = w.credentials_for(hero, session, rel_in, "grants");
    CHECK_FALSE(after.has_key(opens_brass));
    CHECK_FALSE(door.satisfied_by(after));

    // Session grants survive derivation and closure runs over BOTH.
    name_id role_builder = w.intern("builder");
    name_id area_edit = w.intern("area-edit");
    w.key_implies(role_builder, area_edit);
    credentials builder_session;
    builder_session.grant_key(role_builder);
    credentials b = w.credentials_for(hero, builder_session, rel_in, "grants");
    CHECK(b.has_key(role_builder));
    CHECK(b.has_key(area_edit));
}

TEST_CASE("hub::world — containment cycles do not hang credential derivation")
{
    world w;
    entity_id a = w.create("box-a");
    entity_id b = w.create("box-b");
    name_id rel_in = w.intern("in");
    // Degenerate data: two boxes "inside" each other. The walk must
    // terminate and derive nothing.
    w.link_add(a, rel_in, b);
    w.link_add(b, rel_in, a);
    credentials session;
    credentials c = w.credentials_for(a, session, rel_in, "grants");
    CHECK(c.key_count() == 0u);
}
