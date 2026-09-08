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
using madc::hub::pointer_phase;
using madc::hub::entity_id;
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
    row["cls"] = madc::value(std::string("keyword"));
    std::vector<madc::value> spans;
    spans.push_back(madc::value::make_object(row));
    std::map<std::string, madc::value> bad;		// TUI-only (c, no cls): the web skips it
    bad["s"] = madc::value((int64_t)3);
    bad["e"] = madc::value((int64_t)5);
    bad["c"] = madc::value(std::string("bold"));
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
	"[{\"t\":\"ab\",\"s\":[[0,2,\"keyword\"]]},{\"t\":\"cd\",\"s\":[]}]");
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
    row["cls"] = madc::value(std::string("string"));
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
	"[{\"t\":\"abc\",\"s\":[[2,1,\"string\"]]},{\"t\":\"def\",\"s\":[[0,2,\"string\"]]}]"));
    CHECK((*e2)["caret"] == nlohmann::json{ {"line", 1}, {"col", 3} });

    size_t line, col;
    web_line_col("ab\ncd", -5, line, col);
    CHECK(line == 0u);
    CHECK(col == 0u);
    web_line_col("", 3, line, col);
    CHECK(line == 0u);
    CHECK(col == 0u);
}

// An edit node over `text` with the given {s, e, cls} rows, in that order.
static uinode spanned_edit(const roles &r, const char *text,
			   const std::vector<std::vector<long> > &rows,
			   const std::vector<const char *> &classes)
{
    uinode edit(r.edit);
    edit.content = madc::value(std::string(text));
    std::vector<madc::value> spans;
    for ( size_t i = 0; i < rows.size(); ++i )
    {
	std::map<std::string, madc::value> row;
	row["s"] = madc::value((int64_t)rows[i][0]);
	row["e"] = madc::value((int64_t)rows[i][1]);
	row["cls"] = madc::value(std::string(classes[i]));
	spans.push_back(madc::value::make_object(row));
    }
    std::map<std::string, madc::value> h;
    h["spans"] = madc::value::make_array(spans);
    edit.hints = madc::value::make_object(h);
    return edit;
}

TEST_CASE("compose — the span sweep: overlapping, nested and crossing spans clip per line")
{
    world w;
    roles r = roles::standard(w);
    // Lines [0,8) [9,17) [18,26) and the empty line after the trailing
    // newline. `a` spans three lines, `b` nests inside line 1, `c` starts
    // on line 1 and reaches past the end of the text.
    const char *text = "abcdefgh\nijklmnop\nqrstuvwx\n";
    std::vector<std::vector<long> > rows;
    rows.push_back(std::vector<long>{ 0, 20 });
    rows.push_back(std::vector<long>{ 2, 5 });
    rows.push_back(std::vector<long>{ 7, 30 });
    std::vector<const char *> classes;
    classes.push_back("a");
    classes.push_back("b");
    classes.push_back("c");
    nlohmann::json want = nlohmann::json::parse(
	"[{\"t\":\"abcdefgh\",\"s\":[[0,8,\"a\"],[2,3,\"b\"],[7,1,\"c\"]]},"
	"{\"t\":\"ijklmnop\",\"s\":[[0,8,\"a\"],[0,8,\"c\"]]},"
	"{\"t\":\"qrstuvwx\",\"s\":[[0,2,\"a\"],[0,8,\"c\"]]},"
	"{\"t\":\"\",\"s\":[]}]");
    web_model m;
    nlohmann::json ops = nlohmann::json::parse(
	m.compose(r, spanned_edit(r, text, rows, classes)));
    const nlohmann::json *e = node_by_key(ops, "0");
    REQUIRE(e);
    CHECK((*e)["lines"] == want);

    // Rows arriving out of start order emit the same line-DOM: the model
    // orders by start (stably), a contract of the sweep.
    std::vector<std::vector<long> > shuffled;
    shuffled.push_back(rows[2]);
    shuffled.push_back(rows[0]);
    shuffled.push_back(rows[1]);
    std::vector<const char *> shuffled_cls;
    shuffled_cls.push_back("c");
    shuffled_cls.push_back("a");
    shuffled_cls.push_back("b");
    web_model m2;
    ops = nlohmann::json::parse(
	m2.compose(r, spanned_edit(r, text, shuffled, shuffled_cls)));
    e = node_by_key(ops, "0");
    REQUIRE(e);
    CHECK((*e)["lines"] == want);

    // A span ending exactly after a newline gives the next line nothing.
    std::vector<std::vector<long> > tail;
    tail.push_back(std::vector<long>{ 5, 9 });
    std::vector<const char *> tail_cls;
    tail_cls.push_back("d");
    web_model m3;
    ops = nlohmann::json::parse(
	m3.compose(r, spanned_edit(r, text, tail, tail_cls)));
    e = node_by_key(ops, "0");
    REQUIRE(e);
    CHECK((*e)["lines"][0]["s"] == nlohmann::json::parse("[[5,3,\"d\"]]"));
    CHECK((*e)["lines"][1]["s"] == nlohmann::json::parse("[]"));
}

// group -> [heading, edit(text, caret, one keyword span [0,2))]; `extra`
// prepends a content node so the edit's key path shifts from 0.1 to 0.2.
static uinode doc_tree(world &w, const char *text, long caret,
		       bool extra = false, long span_end = 2)
{
    roles r = roles::standard(w);
    uinode root(r.group);
    if ( extra )
    {
	uinode para(r.content);
	para.content = madc::value(std::string("banner"));
	root.add(para);
    }
    uinode head(r.heading);
    head.label = madc::value(std::string("doc"));
    root.add(head);
    uinode edit(r.edit);
    edit.content = madc::value(std::string(text));
    std::map<std::string, madc::value> h;
    h["caret"] = madc::value((int64_t)caret);
    std::map<std::string, madc::value> row;
    row["s"] = madc::value((int64_t)0);
    row["e"] = madc::value((int64_t)span_end);
    row["cls"] = madc::value(std::string("keyword"));
    std::vector<madc::value> spans;
    spans.push_back(madc::value::make_object(row));
    h["spans"] = madc::value::make_array(spans);
    edit.hints = madc::value::make_object(h);
    root.add(edit);
    return root;
}

static const nlohmann::json *edit_op(const nlohmann::json &ops, const char *key = "0.1")
{
    const nlohmann::json *e = node_by_key(ops, key);
    REQUIRE(e);
    REQUIRE((*e)["class"] == "edit");
    return e;
}

// The patch an edit op carries — asserted present first (a const lookup of
// an absent key is a json assertion, not a test failure).
static nlohmann::json patch_of(const nlohmann::json *e)
{
    REQUIRE(e->find("patch") != e->end());
    return (*e)["patch"];
}

TEST_CASE("compose — the edit node is incremental: full first, then one splice, nothing for a caret move")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    // First paint: the full line-DOM and its count.
    nlohmann::json ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    const nlohmann::json *e = edit_op(ops);
    CHECK((*e)["nlines"] == 2);
    CHECK((*e)["lines"] == nlohmann::json::parse(
	"[{\"t\":\"ab\",\"s\":[[0,2,\"keyword\"]]},{\"t\":\"cd\",\"s\":[]}]"));
    CHECK(e->find("patch") == e->end());

    // The same document again: neither lines nor a patch (a resize, a wake).
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    e = edit_op(ops);
    CHECK((*e)["nlines"] == 2);
    CHECK(e->find("lines") == e->end());
    CHECK(e->find("patch") == e->end());

    // A caret move: no line work; the caret field carries it.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 4)));
    e = edit_op(ops);
    CHECK(e->find("lines") == e->end());
    CHECK(e->find("patch") == e->end());
    CHECK((*e)["caret"] == nlohmann::json{ {"line", 1}, {"col", 1} });

    // A span change with the text unchanged is a line change too.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 4, false, 1)));
    e = edit_op(ops);
    CHECK(e->find("lines") == e->end());
    CHECK(patch_of(e) == nlohmann::json::parse(
	"{\"at\":0,\"del\":1,\"ins\":[{\"t\":\"ab\",\"s\":[[0,1,\"keyword\"]]}]}"));

    // One character typed into line 2: replace that one row.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncXd", 5, false, 1)));
    e = edit_op(ops);
    CHECK(e->find("lines") == e->end());
    CHECK((*e)["nlines"] == 2);
    CHECK(patch_of(e) == nlohmann::json::parse(
	"{\"at\":1,\"del\":1,\"ins\":[{\"t\":\"cXd\",\"s\":[]}]}"));

    // Enter inside line 2: one row becomes two.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\nc\nXd", 5, false, 1)));
    e = edit_op(ops);
    CHECK((*e)["nlines"] == 3);
    CHECK(patch_of(e) == nlohmann::json::parse(
	"{\"at\":1,\"del\":1,\"ins\":[{\"t\":\"c\",\"s\":[]},{\"t\":\"Xd\",\"s\":[]}]}"));

    // A line appended at the end: an insertion with nothing deleted.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\nc\nXd\nef", 5, false, 1)));
    e = edit_op(ops);
    CHECK((*e)["nlines"] == 4);
    CHECK(patch_of(e) == nlohmann::json::parse(
	"{\"at\":3,\"del\":0,\"ins\":[{\"t\":\"ef\",\"s\":[]}]}"));

    // The first line deleted. The keyword span rides the document's first
    // byte, so the NEW first line "c" carries it while the old second line
    // "c" did not: the common suffix is Xd/ef, and the splice replaces two
    // old rows with the one re-spanned row.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "c\nXd\nef", 0, false, 1)));
    e = edit_op(ops);
    CHECK((*e)["nlines"] == 3);
    CHECK(patch_of(e) == nlohmann::json::parse(
	"{\"at\":0,\"del\":2,\"ins\":[{\"t\":\"c\",\"s\":[[0,1,\"keyword\"]]}]}"));

    // And a plain first-line deletion with no span in play: pure removal.
    web_model m2;
    ops = nlohmann::json::parse(m2.compose(r, doc_tree(w, "ab\ncd\nef", 0, false, 0)));
    ops = nlohmann::json::parse(m2.compose(r, doc_tree(w, "cd\nef", 0, false, 0)));
    e = edit_op(ops);
    CHECK((*e)["nlines"] == 2);
    CHECK(patch_of(e) == nlohmann::json::parse("{\"at\":0,\"del\":1,\"ins\":[]}"));
}

TEST_CASE("compose — an edit key that moves, a resync, and reset_surface paint in full again")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    nlohmann::json ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    const nlohmann::json *e = edit_op(ops);
    REQUIRE(e->find("lines") != e->end());

    // A node inserted before the editor moves its key (0.1 -> 0.2): a new
    // key paints in full, and the old key's basis is dropped with it.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0, true)));
    e = edit_op(ops, "0.2");
    CHECK(e->find("lines") != e->end());
    CHECK(node_by_key(ops, "0.1")->at("class") == "heading");
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    e = edit_op(ops);
    CHECK(e->find("lines") != e->end());		// 0.1 forgot — full again
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    e = edit_op(ops);
    CHECK(e->find("lines") == e->end());		// and steady once more

    // The page's resync: one focus event (the application recomposes), and
    // the next compose paints every edit node in full.
    std::vector<tui_event> ev = m.apply_input("{\"kind\":\"resync\"}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    e = edit_op(ops);
    CHECK(e->find("lines") != e->end());
    CHECK((*e)["nlines"] == 2);

    // ui::refresh's reset does the same.
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    CHECK(edit_op(ops)->find("lines") == edit_op(ops)->end());
    m.reset_surface();
    ops = nlohmann::json::parse(m.compose(r, doc_tree(w, "ab\ncd", 0)));
    CHECK(edit_op(ops)->find("lines") != edit_op(ops)->end());
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

// A workbench tree: a status group with region "statusbar", an editor
// group with region "editor" + a tab strip, and a docked sidebar plus a
// floating popup. web_model emits region/tabs/popup as additive op fields
// the page places by (slice 3, madcide GUI plan Task 2).
static uinode regioned(world &w, const char *region, bool tabs = false,
		       bool popup = false)
{
    roles r = roles::standard(w);
    uinode g(r.group);
    std::map<std::string, madc::value> h;
    if ( region )
	h["region"] = madc::value(std::string(region));
    if ( tabs )
	h["tabs"] = madc::value((int64_t)1);
    if ( popup )
	h["popup"] = madc::value((int64_t)1);
    g.hints = madc::value::make_object(h);
    return g;
}

TEST_CASE("compose — region / tabs / popup are additive op fields")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    uinode root(r.group);
    root.add(regioned(w, "statusbar"));		// 0.0
    root.add(regioned(w, "editor", true));	// 0.1 (tab strip)
    root.add(regioned(w, "sidebar"));		// 0.2 (docked)
    root.add(regioned(w, NULL, false, true));	// 0.3 (floating popup)

    nlohmann::json ops = nlohmann::json::parse(m.compose(r, root), nullptr, false);
    REQUIRE(!ops.is_discarded());

    const nlohmann::json *sb = node_by_key(ops, "0.0");
    REQUIRE(sb);
    CHECK((*sb)["region"] == "statusbar");
    CHECK((*sb).find("tabs") == (*sb).end());	// no tab strip on the bar
    CHECK((*sb).find("popup") == (*sb).end());

    const nlohmann::json *ed = node_by_key(ops, "0.1");
    REQUIRE(ed);
    CHECK((*ed)["region"] == "editor");
    CHECK((*ed)["tabs"] == true);

    const nlohmann::json *side = node_by_key(ops, "0.2");
    REQUIRE(side);
    CHECK((*side)["region"] == "sidebar");

    const nlohmann::json *pop = node_by_key(ops, "0.3");
    REQUIRE(pop);
    CHECK((*pop)["popup"] == true);
    CHECK((*pop).find("region") == (*pop).end());
}

TEST_CASE("compose — a node without layout hints carries no region/popup/tabs (negative control)")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    nlohmann::json ops = nlohmann::json::parse(m.compose(r, editor_tree(w, 3)),
					       nullptr, false);
    REQUIRE(!ops.is_discarded());
    for ( size_t i = 0; i < ops.size(); ++i )
    {
	if ( ops[i].value("op", "") != "node" )
	    continue;
	CHECK(ops[i].find("region") == ops[i].end());
	CHECK(ops[i].find("popup") == ops[i].end());
	CHECK(ops[i].find("tabs") == ops[i].end());
    }
}

TEST_CASE("compose — a status node with items renders a left/right item bar")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    uinode root(r.group);
    uinode status(r.status);
    status.content = madc::value(std::string("Ln 1        Row 2"));  // TUI string
    std::map<std::string, madc::value> items;
    items["left"] = madc::value(std::string("Ln 1"));
    items["right"] = madc::value(std::string("Row 2"));
    std::map<std::string, madc::value> h;
    h["items"] = madc::value::make_object(items);
    status.hints = madc::value::make_object(h);
    root.add(status);

    nlohmann::json ops = nlohmann::json::parse(m.compose(r, root), nullptr, false);
    REQUIRE(!ops.is_discarded());
    const nlohmann::json *st = node_by_key(ops, "0.0");
    REQUIRE(st);
    CHECK((*st)["class"] == "status");
    CHECK((*st)["text"] == "Ln 1        Row 2");	// the TUI combined string
    REQUIRE((*st).contains("items"));
    CHECK((*st)["items"]["left"] == "Ln 1");
    CHECK((*st)["items"]["right"] == "Row 2");

    // A status without items carries none (negative control).
    web_model m2;
    uinode root2(r.group);
    uinode plain(r.status);
    plain.content = madc::value(std::string("just text"));
    root2.add(plain);
    nlohmann::json ops2 = nlohmann::json::parse(m2.compose(r, root2), nullptr, false);
    const nlohmann::json *st2 = node_by_key(ops2, "0.0");
    REQUIRE(st2);
    CHECK((*st2)["text"] == "just text");
    CHECK((*st2).find("items") == (*st2).end());
}

// A bare edit node projecting `subject` — the pointer event's identity.
static uinode pointer_tree(world &w, const char *text, entity_id subject)
{
    roles r = roles::standard(w);
    uinode root(r.group);
    uinode edit(r.edit);
    edit.content = madc::value(std::string(text));
    edit.subject = subject;
    root.add(edit);
    return root;
}

TEST_CASE("apply_input — a pointer gesture resolves to a byte offset over the node's rows; focus follows it")
{
    world w;
    roles r = roles::standard(w);
    web_model m;
    m.compose(r, doc_tree(w, "ab\ncd", 0));		// edit key "0.1": rows ab / cd

    std::vector<tui_event> ev = m.apply_input(
	"{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.1\",\"line\":1,\"col\":1}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::pointer);
    CHECK(ev[0].phase == pointer_phase::down);
    CHECK(ev[0].offset == 4);			// "ab\n" + 1
    CHECK(ev[0].subject == 0);
    // Past the end of a line: its end. Past the last line: the text's end.
    // Before the first line: 0. Drag and up carry their phase.
    ev = m.apply_input("{\"kind\":\"pointer\",\"phase\":\"drag\",\"key\":\"0.1\",\"line\":0,\"col\":9}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].phase == pointer_phase::drag);
    CHECK(ev[0].offset == 2);
    ev = m.apply_input("{\"kind\":\"pointer\",\"phase\":\"up\",\"key\":\"0.1\",\"line\":7,\"col\":0}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].phase == pointer_phase::up);
    CHECK(ev[0].offset == 5);
    ev = m.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.1\",\"line\":-3,\"col\":4}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].offset == 0);
    // A key with no basis (a node that is not an edit, a pruned one), a
    // phase outside the vocabulary, a missing field: nothing, no throw.
    CHECK(m.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.0\",\"line\":0,\"col\":0}").empty());
    CHECK(m.apply_input("{\"kind\":\"pointer\",\"phase\":\"tap\",\"key\":\"0.1\",\"line\":0,\"col\":0}").empty());
    CHECK(m.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.1\",\"line\":0}").empty());

    // Columns arrive as UTF-16 units (the page's string indices) and leave
    // as bytes: a two-byte é is one unit, a four-byte emoji two (a column
    // inside the pair snaps to its start). The node's subject rides along.
    web_model m2;
    m2.compose(r, pointer_tree(w, "h\xC3\xA9llo\n\xF0\x9F\x98\x80" "ab", 42));
    ev = m2.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.0\",\"line\":0,\"col\":2}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].offset == 3);
    CHECK(ev[0].subject == 42u);
    ev = m2.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.0\",\"line\":1,\"col\":2}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].offset == 7 + 4);
    ev = m2.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.0\",\"line\":1,\"col\":1}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].offset == 7);
    ev = m2.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.0\",\"line\":1,\"col\":3}");
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].offset == 7 + 5);

    // A press is a focus gesture: with the menu autofocused, a pointer on
    // the edit moves the focus slot to it (the next keys are its).
    web_model m3;
    m3.compose(r, editor_tree(w, 0, -1, -1, true));	// edit = slot 0, menu = slot 1 (autofocus)
    CHECK(m3.focus_slot() == 1u);
    ev = m3.apply_input("{\"kind\":\"pointer\",\"phase\":\"down\",\"key\":\"0.2\",\"line\":0,\"col\":0}");
    REQUIRE(ev.size() == 1u);
    CHECK(m3.focus_slot() == 0u);
}
