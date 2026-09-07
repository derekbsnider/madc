// Unit battery for madcdis/web_model.h — the level-3 renderer's model (the
// web-provider engine plan, Task 3): the value tree as keyed DOM operations
// (JSON through the repo's one JSON owner), and the page's posted input as
// the SAME semantic events the grid model emits — chords through
// key_resolver, navigation through focus_state, the loop through
// ui_apply_keys. Plan: docs/plans/2026-09-07-web-provider-engine-plan.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <map>
#include <string>
#include <vector>

#include "madcdis/web_model.h"

using madc::hub::world;
using madc::hub::roles;
using madc::hub::uinode;
using madc::hub::name_id;
using madc::hub::tui_key;
using madc::hub::tui_keyev;
using madc::hub::tui_event;
using madc::hub::tui_event_kind;
using madc::hub::tui_bindings;
using madc::hub::web_model;
using madc::hub::web_line_col;

static uinode option(world &w, const char *text, const char *action)
{
    roles r = roles::standard(w);
    uinode o(r.item);
    o.content = madc::value(std::string(text));
    if ( action )
	o.actions.push_back(w.intern(action));
    return o;
}

// heading + content + edit("ab\ncd", caret 3, one kw span) + choice(2)
static uinode editor_tree(world &w, long caret, long sel_start = -1,
			  long sel_end = -1, bool autofocus_menu = false)
{
    roles r = roles::standard(w);
    uinode root(r.group);
    uinode head(r.heading);
    head.label = madc::value(std::string("notes.txt"));
    head.content = madc::value(std::string("[+]"));
    root.add(head);
    uinode para(r.content);
    para.content = madc::value(std::string("hello \"world\""));
    root.add(para);
    uinode edit(r.edit);
    edit.content = madc::value(std::string("ab\ncd"));
    std::map<std::string, madc::value> h;
    h["caret"] = madc::value((int64_t)caret);
    if ( sel_start >= 0 )
    {
	h["sel_start"] = madc::value((int64_t)sel_start);
	h["sel_end"] = madc::value((int64_t)sel_end);
    }
    std::map<std::string, madc::value> row;
    row["s"] = madc::value((int64_t)0);
    row["e"] = madc::value((int64_t)2);
    row["c"] = madc::value(std::string("kw"));
    std::vector<madc::value> spans;
    spans.push_back(madc::value::make_object(row));
    std::map<std::string, madc::value> bad;		// malformed: skipped
    bad["s"] = madc::value((int64_t)3);
    spans.push_back(madc::value::make_object(bad));
    h["spans"] = madc::value::make_array(spans);
    edit.hints = madc::value::make_object(h);
    root.add(edit);
    uinode menu(r.choice);
    menu.add(option(w, "Save", "w"));
    menu.add(option(w, "Quit", "q"));
    if ( autofocus_menu )
    {
	std::map<std::string, madc::value> mh;
	mh["focus"] = madc::value((int64_t)1);
	menu.hints = madc::value::make_object(mh);
    }
    root.add(menu);
    return root;
}

static const nlohmann::json *node_by_key(const nlohmann::json &ops,
					 const std::string &key)
{
    for ( size_t i = 0; i < ops.size(); ++i )
	if ( ops[i].value("op", "") == "node" && ops[i].value("key", "") == key )
	    return &ops[i];
    return NULL;
}

TEST_CASE("compose — keyed DOM ops: root, one node per tree node, end")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    std::string text = m.compose(r, editor_tree(w, 3));
    nlohmann::json ops = nlohmann::json::parse(text, nullptr, false);
    REQUIRE(!ops.is_discarded());		// well-formed JSON
    REQUIRE(ops.is_array());
    REQUIRE(ops.size() == 7u);			// root + 5 nodes + end
    CHECK(ops.front() == nlohmann::json{ {"op", "root"} });
    CHECK(ops.back() == nlohmann::json{ {"op", "end"} });

    const nlohmann::json *root = node_by_key(ops, "0");
    REQUIRE(root);
    CHECK((*root)["class"] == "group");
    CHECK((*root)["parent"] == "");

    const nlohmann::json *head = node_by_key(ops, "0.0");
    REQUIRE(head);
    CHECK((*head)["class"] == "heading");
    CHECK((*head)["parent"] == "0");
    CHECK((*head)["label"] == "notes.txt");
    CHECK((*head)["text"] == "[+]");

    const nlohmann::json *para = node_by_key(ops, "0.1");
    REQUIRE(para);
    CHECK((*para)["class"] == "content");
    CHECK((*para)["text"] == "hello \"world\"");	// escaped by the owner
    CHECK(text.find("hello \\\"world\\\"") != std::string::npos);

    const nlohmann::json *edit = node_by_key(ops, "0.2");
    REQUIRE(edit);
    CHECK((*edit)["class"] == "edit");
    nlohmann::json want_lines = nlohmann::json::parse(
	"[{\"t\":\"ab\",\"s\":[[0,2,\"kw\"]]},{\"t\":\"cd\",\"s\":[]}]");
    CHECK((*edit)["lines"] == want_lines);
    CHECK((*edit)["caret"] == nlohmann::json{ {"line", 1}, {"col", 0} });
    CHECK((*edit)["sel"].is_null());
    CHECK((*edit)["tabwidth"] == 8);
    CHECK((*edit)["focus"] == true);		// first focusable: slot 0

    const nlohmann::json *menu = node_by_key(ops, "0.3");
    REQUIRE(menu);
    CHECK((*menu)["class"] == "choice");
    CHECK((*menu)["opts"] == nlohmann::json::parse("[\"Save\",\"Quit\"]"));
    CHECK((*menu)["sel"] == 0);
    CHECK((*menu)["list"] == false);
    CHECK((*menu)["focus"] == false);
    CHECK(!node_by_key(ops, "0.3.0"));		// options consumed, no recursion

    REQUIRE(m.focusables().size() == 2u);
    CHECK(m.focus_slot() == 0u);
}

TEST_CASE("compose — selection spans lines; a span across lines splits; autofocus")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    // sel [1, 4) covers "b\nc": (0,1)..(1,1)
    nlohmann::json ops = nlohmann::json::parse(
	m.compose(r, editor_tree(w, 4, 1, 4, true)));
    const nlohmann::json *edit = node_by_key(ops, "0.2");
    REQUIRE(edit);
    CHECK((*edit)["sel"] == nlohmann::json::parse("[[0,1],[1,1]]"));
    CHECK((*edit)["caret"] == nlohmann::json{ {"line", 1}, {"col", 1} });
    CHECK((*edit)["focus"] == false);		// the menu carried focus:1
    const nlohmann::json *menu = node_by_key(ops, "0.3");
    REQUIRE(menu);
    CHECK((*menu)["focus"] == true);
    CHECK(m.focus_slot() == 1u);

    // A span crossing the newline appears on both lines, clipped.
    uinode edit_node(r.edit);
    edit_node.content = madc::value(std::string("abc\ndef"));
    std::map<std::string, madc::value> row;
    row["s"] = madc::value((int64_t)2);
    row["e"] = madc::value((int64_t)6);
    row["c"] = madc::value(std::string("str"));
    std::vector<madc::value> spans;
    spans.push_back(madc::value::make_object(row));
    std::map<std::string, madc::value> h;
    h["spans"] = madc::value::make_array(spans);
    h["caret"] = madc::value((int64_t)99);	// past the end: clamps
    edit_node.hints = madc::value::make_object(h);
    web_model m2;
    ops = nlohmann::json::parse(m2.compose(r, edit_node));
    const nlohmann::json *e2 = node_by_key(ops, "0");
    REQUIRE(e2);
    CHECK((*e2)["lines"] == nlohmann::json::parse(
	"[{\"t\":\"abc\",\"s\":[[2,1,\"str\"]]},{\"t\":\"def\",\"s\":[[0,2,\"str\"]]}]"));
    CHECK((*e2)["caret"] == nlohmann::json{ {"line", 1}, {"col", 3} });

    size_t line, col;
    web_line_col("ab\ncd", -5, line, col);
    CHECK(line == 0u);
    CHECK(col == 0u);
    web_line_col("", 3, line, col);
    CHECK(line == 0u);
    CHECK(col == 0u);
}

TEST_CASE("apply_input — text, keys, chords: the grid's events from the page's input")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    m.compose(r, editor_tree(w, 0));

    std::vector<tui_event> ev = m.apply_input("{\"kind\":\"text\",\"text\":\"hi\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::text);
    CHECK(ev[0].text == "hi");

    // A non-printable with no binding: the application's key event, and
    // an edit is focused so it carries no choice selection.
    ev = m.apply_input("{\"kind\":\"key\",\"key\":\"del\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::key);
    CHECK(ev[0].key == tui_key::del);
    CHECK(!ev[0].choice_focused);

    // Chords resolve in the shared owner: ^k opens (focus = repaint the
    // %k echo), a printable posted as TEXT completes it — the rest of the
    // run is text.
    tui_bindings b;
    b.bind("^k s", "save");
    b.bind("^q", "quit");
    std::string err;
    REQUIRE(b.finalize(err));
    m.set_bindings(b);
    ev = m.apply_input("{\"kind\":\"key\",\"key\":\"^k\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(m.pending_chord() == "^k");
    ev = m.apply_input("{\"kind\":\"text\",\"text\":\"sx\"}");
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "save");
    CHECK(ev[0].seq == "^k s");
    CHECK(ev[1].kind == tui_event_kind::text);
    CHECK(ev[1].text == "x");
    CHECK(m.pending_chord().empty());
    // A printable posted as a KEY spelling is the same key.
    m.apply_input("{\"kind\":\"key\",\"key\":\"^k\"}");
    ev = m.apply_input("{\"kind\":\"key\",\"key\":\"S\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "save");
    ev = m.apply_input("{\"kind\":\"key\",\"key\":\"^q\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "quit");
    // esc cancels; a resize mid-chord is transparent.
    m.apply_input("{\"kind\":\"key\",\"key\":\"^k\"}");
    ev = m.apply_input("{\"kind\":\"resize\",\"rows\":50,\"cols\":132}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::resize);
    CHECK(m.pending_chord() == "^k");
    ev = m.apply_input("{\"kind\":\"key\",\"key\":\"esc\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(m.pending_chord().empty());
}

TEST_CASE("apply_input — navigation through the focus owner; viewport facts; snapshot")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    m.compose(r, editor_tree(w, 0, -1, -1, true));	// the menu has focus
    REQUIRE(m.focus_slot() == 1u);
    std::vector<tui_event> ev = m.apply_input("{\"kind\":\"key\",\"key\":\"right\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(m.selection_of(1) == 1u);
    // A recompose shows the moved selection on the choice node.
    nlohmann::json ops = nlohmann::json::parse(
	m.compose(r, editor_tree(w, 0, -1, -1, true)));
    const nlohmann::json *menu = node_by_key(ops, "0.3");
    REQUIRE(menu);
    CHECK((*menu)["sel"] == 1);
    ev = m.apply_input("{\"kind\":\"key\",\"key\":\"enter\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::choose);
    CHECK(ev[0].option == 1u);
    CHECK(ev[0].action == w.intern("q"));
    // tab moves to the edit; a key there rides through without a selection.
    ev = m.apply_input("{\"kind\":\"key\",\"key\":\"tab\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(m.focus_slot() == 0u);

    CHECK(m.rows() == 24u);
    CHECK(m.cols() == 80u);
    ev = m.apply_input("{\"kind\":\"resize\",\"rows\":50,\"cols\":132}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::resize);
    CHECK(m.rows() == 50u);
    CHECK(m.cols() == 132u);

    ev = m.apply_input("{\"kind\":\"snapshot\",\"text\":\"Ln 1\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::snapshot);
    CHECK(ev[0].text == "Ln 1");
    CHECK(m.last_snapshot() == "Ln 1");
}

TEST_CASE("apply_input — malformed or unknown input yields no events and never throws")
{
    web_model m;
    CHECK(m.apply_input("").empty());
    CHECK(m.apply_input("{").empty());
    CHECK(m.apply_input("[1,2]").empty());
    CHECK(m.apply_input("{\"kind\":\"dance\"}").empty());
    CHECK(m.apply_input("{\"kind\":\"key\"}").empty());
    CHECK(m.apply_input("{\"kind\":\"key\",\"key\":\"nosuch\"}").empty());
    CHECK(m.apply_input("{\"kind\":\"text\",\"text\":7}").empty());
    CHECK(m.apply_input("{\"kind\":\"resize\",\"rows\":0,\"cols\":80}").empty());
    CHECK(m.apply_input("{\"kind\":\"resize\",\"rows\":\"a\",\"cols\":80}").empty());
    CHECK(m.rows() == 24u);
    CHECK(m.apply_input("{\"kind\":\"snapshot\"}").empty());
    CHECK(m.last_snapshot().empty());
}
