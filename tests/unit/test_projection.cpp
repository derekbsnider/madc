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

TEST_CASE("choice — the node's children render as a numbered menu")
{
    world w;
    roles r = roles::standard(w);

    uinode menu(r.choice);
    menu.label = madc::value("Which way?");
    uinode north(r.item);
    north.content = madc::value("Go north");
    north.actions.push_back(w.intern("go"));
    menu.add(north);
    uinode east(r.item);
    east.label = madc::value("Go east");	// content-else-label, like item
    menu.add(east);
    // Detail UNDER an option still renders generically, after its number.
    uinode down(r.item);
    down.content = madc::value("Climb down");
    uinode warn(r.status);
    warn.content = madc::value("It looks slippery.");
    down.add(warn);
    menu.add(down);

    CHECK(render_text(r, menu, 60) ==
	  "Which way?\n"
	  "  1. Go north\n"
	  "  2. Go east\n"
	  "  3. Climb down\n"
	  "It looks slippery.\n");

    // No label: just the numbered options — and options are CONSUMED by
    // the menu (no generic double-render of the children).
    uinode bare(r.choice);
    uinode only(r.item);
    only.content = madc::value("Yes");
    bare.add(only);
    CHECK(render_text(r, bare, 60) == "  1. Yes\n");
}

TEST_CASE("uinode <-> value — the projection tree as hub data, names not ids")
{
    world w;
    roles r = roles::standard(w);

    uinode menu(r.choice);
    menu.label = madc::value("Pick");
    menu.subject = w.create("signpost");
    menu.states.push_back(w.intern("focused"));
    uinode opt(r.item);
    opt.content = madc::value("Go north");
    opt.actions.push_back(w.intern("go"));
    menu.add(opt);

    madc::value v = madc::hub::uinode_to_value(w, menu);
    REQUIRE(v.is_object());
    const std::map<std::string, madc::value> &o = v.as_object();
    CHECK(o.at("role").as_string() == "choice");
    CHECK(o.at("label").as_string() == "Pick");
    CHECK(o.at("subject").as_integer() == (int64_t)menu.subject);
    REQUIRE(o.count("states") == 1u);
    CHECK(o.at("states").as_array()[0].as_string() == "focused");
    REQUIRE(o.count("children") == 1u);
    const std::map<std::string, madc::value> &co
	= o.at("children").as_array()[0].as_object();
    CHECK(co.at("role").as_string() == "item");
    CHECK(co.at("actions").as_array()[0].as_string() == "go");
    // Sparse: an absent field is absent, not a null placeholder.
    CHECK(co.count("children") == 0u);
    CHECK(co.count("subject") == 0u);

    // Roundtrip: the value tree interns back to an equivalent uinode.
    uinode back = madc::hub::value_to_uinode(w, v);
    CHECK(back.role == r.choice);
    CHECK(prose::text_of(back.label) == "Pick");
    CHECK(back.subject == menu.subject);
    REQUIRE(back.states.size() == 1u);
    CHECK(back.states[0] == w.intern("focused"));
    REQUIRE(back.children.size() == 1u);
    CHECK(back.children[0].role == r.item);
    REQUIRE(back.children[0].actions.size() == 1u);
    CHECK(back.children[0].actions[0] == w.intern("go"));
    CHECK(render_text(r, back, 60) ==
	  "Pick\n"
	  "  1. Go north\n");

    // Tolerant reader: a bare text value is the smallest leaf — content.
    uinode leaf = madc::hub::value_to_uinode(w, madc::value("hi"));
    CHECK(leaf.role == 0);
    CHECK(prose::text_of(leaf.content) == "hi");
    // Wrong-kind fields are skipped, never a crash.
    madc::value junk = madc::value::make_object();
    junk.object()["role"] = madc::value((int64_t)7);
    junk.object()["children"] = madc::value("not an array");
    uinode j = madc::hub::value_to_uinode(w, junk);
    CHECK(j.role == 0);
    CHECK(j.children.empty());
}
