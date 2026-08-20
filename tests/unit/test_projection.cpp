// Unit battery for the Track 7 Phase 1 S3 projection stack: the value-typed
// semantic tree (uinode), the prose kit, the level-0 typesetter, and the
// generic inspect() projection.
// Plan: docs/plans/2026-08-20-track7-phase1-text-adventure.md (S3).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madcdis/projection.h"
#include "madcdis/render_text.h"

using madc::hub::world;
using madc::hub::entity_id;
using madc::hub::name_id;
using madc::hub::roles;
using madc::hub::uinode;
using madc::hub::wrap_text;
using madc::hub::render_text;
using madc::hub::inspect;
namespace prose = madc::hub::prose;

TEST_CASE("prose — articles, plurals, enumeration, sentences, text_of")
{
    CHECK(prose::article("lantern") == "a lantern");
    CHECK(prose::article("apple") == "an apple");
    CHECK(prose::article("Iron key") == "an Iron key");
    CHECK(prose::article("") == "");

    CHECK(prose::plural(1, "lamp", "lamps") == "lamp");
    CHECK(prose::plural(3, "lamp", "lamps") == "lamps");
    CHECK(prose::plural(0, "lamp", "lamps") == "lamps");

    std::vector<std::string> one;
    one.push_back("a lantern");
    CHECK(prose::enumerate(one) == "a lantern");
    std::vector<std::string> two = one;
    two.push_back("a key");
    CHECK(prose::enumerate(two) == "a lantern and a key");
    std::vector<std::string> three = two;
    three.push_back("an apple");
    CHECK(prose::enumerate(three) == "a lantern, a key and an apple");
    CHECK(prose::enumerate(std::vector<std::string>()) == "");
    CHECK(prose::enumerate(two, "or") == "a lantern or a key");

    CHECK(prose::sentence("taken") == "Taken.");
    CHECK(prose::sentence("It is dark!") == "It is dark!");
    CHECK(prose::sentence("") == "");

    CHECK(prose::text_of(madc::value("hi")) == "hi");
    CHECK(prose::text_of(madc::value((int64_t)42)) == "42");
    CHECK(prose::text_of(madc::value(true)) == "true");
    CHECK(prose::text_of(madc::value()) == "");
}

TEST_CASE("wrap_text — greedy wrap honours width and explicit newlines")
{
    CHECK(wrap_text("twisty little passages all alike", 14)
	  == "twisty little\npassages all\nalike");
    CHECK(wrap_text("short", 20) == "short");
    CHECK(wrap_text("one\ntwo three", 20) == "one\ntwo three");
    // A single overlong word is not broken, only isolated on its line.
    CHECK(wrap_text("hi supercalifragilistic yes", 8)
	  == "hi\nsupercalifragilistic\nyes");
}

TEST_CASE("render_text — typesetting only, role-driven, byte-exact")
{
    world w;
    roles r = roles::standard(w);

    uinode root(r.group);
    uinode head(r.heading);
    head.label = madc::value("Twisty Passage");
    root.add(head);
    uinode body(r.content);
    body.content = madc::value(
	"You are in a maze of twisty little passages, all alike.");
    root.add(body);
    uinode things(r.list);
    uinode lamp(r.item);
    lamp.content = madc::value("A brass lantern is here.");
    things.add(lamp);
    root.add(things);
    uinode stat(r.status);
    stat.content = madc::value("Exits: north and east.");
    root.add(stat);
    uinode act(r.action);
    act.label = madc::value("look");
    root.add(act);

    std::string text = render_text(r, root, 60);
    CHECK(text ==
	  "=== Twisty Passage ===\n"
	  "You are in a maze of twisty little passages, all alike.\n"
	  "  A brass lantern is here.\n"
	  "Exits: north and east.\n"
	  "[look]\n");
}

TEST_CASE("inspect — the generic entity browser: properties + links")
{
    world w;
    roles r = roles::standard(w);
    entity_id room = w.create("twisty-passage");
    entity_id lamp = w.create("brass-lantern");
    name_id rel_in = w.intern("in");
    name_id rel_exit = w.intern("exit");
    name_id north = w.intern("north");
    entity_id other = w.create("dead-end");

    w.get(lamp)->bag.object()["fuel"] = madc::value((int64_t)50);
    w.get(lamp)->bag.object()["lit"] = madc::value(false);
    w.get(lamp)->bag.object()["desc"] = madc::value("A dented brass lantern.");
    w.link_add(lamp, rel_in, room);
    w.link_add(room, rel_exit, other, north);

    uinode tree = inspect(w, r, lamp);
    CHECK(tree.subject == lamp);
    REQUIRE(tree.children.size() == 3u);	// heading, properties, links
    CHECK(tree.children[0].role == r.heading);
    CHECK(prose::text_of(tree.children[0].label) == "brass-lantern");

    const uinode &props = tree.children[1];
    REQUIRE(props.children.size() == 3u);	// map order: desc, fuel, lit
    CHECK(prose::text_of(props.children[0].content)
	  == "desc = A dented brass lantern.");
    CHECK(prose::text_of(props.children[1].content) == "fuel = 50");
    CHECK(prose::text_of(props.children[2].content) == "lit = false");

    const uinode &edges = tree.children[2];
    REQUIRE(edges.children.size() == 1u);
    CHECK(prose::text_of(edges.children[0].content)
	  == "brass-lantern -in-> twisty-passage");

    // The room's inspector shows the keyed exit with its key spelling.
    uinode room_tree = inspect(w, r, room);
    const uinode &room_edges = room_tree.children[2];
    REQUIRE(room_edges.children.size() == 2u);	// the lamp link + the exit
    CHECK(prose::text_of(room_edges.children[1].content)
	  == "twisty-passage -exit[north]-> dead-end");

    // Absent entity: a loud status node, never a crash.
    uinode missing = inspect(w, r, (entity_id)999);
    REQUIRE(missing.children.size() == 1u);
    CHECK(prose::text_of(missing.children[0].content) == "no such entity");

    // Rendering the inspector goes through the same level-0 typesetter.
    std::string text = render_text(r, tree, 78);
    CHECK(text.find("=== brass-lantern ===") != std::string::npos);
    CHECK(text.find("  fuel = 50") != std::string::npos);
}
