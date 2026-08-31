// Unit battery for madcdis/tui_model.h — the level-1 renderer's model
// (Track 7.2 R5): byte->key escape parsing, key->semantic-event
// adaptation with printable-run coalescing (§7.5), tree->grid layout,
// choice navigation (design success criterion 4's TUI half), edit-window
// scrolling/caret/selection, and dirty-row differencing.
// Plan: docs/plans/2026-08-24-ui-interaction-rework-and-texteditor.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstring>
#include <string>
#include <vector>

#include "madcdis/tui_model.h"

using madc::hub::world;
using madc::hub::roles;
using madc::hub::uinode;
using madc::hub::name_id;
using madc::hub::tui_attr;
using madc::hub::tui_grid;
using madc::hub::tui_key;
using madc::hub::tui_keyev;
using madc::hub::tui_keyparse;
using madc::hub::tui_event;
using madc::hub::tui_event_kind;
using madc::hub::tui_model;
using madc::hub::tui_dirty_rows;
using madc::hub::tui_paint_plan;
using madc::hub::tui_diff_plan;
using madc::hub::tui_bindings;
using madc::hub::tui_key_name;
using madc::hub::tui_key_from_name;

static std::vector<tui_keyev> parse(const char *bytes, bool flush = true)
{
    tui_keyparse p;
    std::vector<tui_keyev> out;
    p.feed(bytes, strlen(bytes), out);
    if ( flush )
	p.flush(out);
    return out;
}

TEST_CASE("keyparse — printables, controls, enter/tab/backspace")
{
    std::vector<tui_keyev> k = parse("hi");
    REQUIRE(k.size() == 2u);
    CHECK(k[0].kind == tui_key::ch);
    CHECK(k[0].ch == 'h');
    CHECK(k[1].ch == 'i');

    k = parse("\x13");			// ^S
    REQUIRE(k.size() == 1u);
    CHECK(k[0].kind == tui_key::ctrl);
    CHECK(k[0].ch == 's');

    k = parse("\r\t\x7f\x08");
    REQUIRE(k.size() == 4u);
    CHECK(k[0].kind == tui_key::enter);
    CHECK(k[1].kind == tui_key::tab);
    CHECK(k[2].kind == tui_key::backspace);
    CHECK(k[3].kind == tui_key::backspace);

    // The punctuation controls 0x1c..0x1f (JOE's ^_ undo / ^^ redo).
    k = parse("\x1c\x1d\x1e\x1f");
    REQUIRE(k.size() == 4u);
    CHECK(k[0].kind == tui_key::ctrl);
    CHECK(k[0].ch == '\\');
    CHECK(k[1].ch == ']');
    CHECK(k[2].ch == '^');
    CHECK(k[3].ch == '_');
}

TEST_CASE("keyparse — CSI and SS3 escape sequences, tilde codes, bare ESC")
{
    std::vector<tui_keyev> k = parse("\x1b[A\x1b[B\x1b[C\x1b[D");
    REQUIRE(k.size() == 4u);
    CHECK(k[0].kind == tui_key::up);
    CHECK(k[1].kind == tui_key::down);
    CHECK(k[2].kind == tui_key::right);
    CHECK(k[3].kind == tui_key::left);

    k = parse("\x1b[H\x1b[F\x1bOH\x1bOF\x1bOA");
    REQUIRE(k.size() == 5u);
    CHECK(k[0].kind == tui_key::home);
    CHECK(k[1].kind == tui_key::end);
    CHECK(k[2].kind == tui_key::home);
    CHECK(k[3].kind == tui_key::end);
    CHECK(k[4].kind == tui_key::up);

    k = parse("\x1b[3~\x1b[5~\x1b[6~\x1b[1~\x1b[4~\x1b[2~");
    REQUIRE(k.size() == 6u);
    CHECK(k[0].kind == tui_key::del);
    CHECK(k[1].kind == tui_key::pgup);
    CHECK(k[2].kind == tui_key::pgdn);
    CHECK(k[3].kind == tui_key::home);
    CHECK(k[4].kind == tui_key::end);
    CHECK(k[5].kind == tui_key::ins);

    // A modifier-parameterized arrow resolves to the unmodified key.
    k = parse("\x1b[1;2A");
    REQUIRE(k.size() == 1u);
    CHECK(k[0].kind == tui_key::up);

    // Bare ESC resolves only at the input pause (flush).
    tui_keyparse p;
    std::vector<tui_keyev> out;
    p.feed("\x1b", 1, out);
    CHECK(out.empty());
    p.flush(out);
    REQUIRE(out.size() == 1u);
    CHECK(out[0].kind == tui_key::esc);

    // ESC followed by an ordinary byte: esc, then the byte.
    k = parse("\x1bq");
    REQUIRE(k.size() == 2u);
    CHECK(k[0].kind == tui_key::esc);
    CHECK(k[1].kind == tui_key::ch);
    CHECK(k[1].ch == 'q');

    // A partial CSI at the pause is dropped, and parsing recovers.
    tui_keyparse q;
    out.clear();
    q.feed("\x1b[5", 3, out);
    q.flush(out);
    CHECK(out.empty());
    q.feed("x", 1, out);
    REQUIRE(out.size() == 1u);
    CHECK(out[0].ch == 'x');
}

// A world exists only to intern the role/action vocabulary.
static uinode option(world &w, const char *text, const char *action)
{
    roles r = roles::standard(w);
    uinode o(r.item);
    o.content = madc::value(std::string(text));
    if ( action )
	o.actions.push_back(w.intern(action));
    return o;
}

static uinode editor_tree(world &w, const std::string &doc, long caret,
			  long sel_start = -1, long sel_end = -1,
			  long tabw = 0)
{
    roles r = roles::standard(w);
    uinode root(r.group);
    uinode head(r.heading);
    head.label = madc::value(std::string("notes.txt"));
    head.content = madc::value(std::string("[+]"));
    root.add(head);
    uinode edit(r.edit);
    edit.content = madc::value(doc);
    std::map<std::string, madc::value> h;
    h["caret"] = madc::value((int64_t)caret);
    if ( sel_start >= 0 )
    {
	h["sel_start"] = madc::value((int64_t)sel_start);
	h["sel_end"] = madc::value((int64_t)sel_end);
    }
    if ( tabw > 0 )
	h["tabwidth"] = madc::value((int64_t)tabw);
    edit.hints = madc::value::make_object(h);
    root.add(edit);
    uinode status(r.status);
    status.content = madc::value(std::string("Ln 1"));
    root.add(status);
    uinode menu(r.choice);
    menu.add(option(w, "Save", "w"));
    menu.add(option(w, "Find", "f"));
    menu.add(option(w, "Quit", "q"));
    root.add(menu);
    return root;
}

TEST_CASE("compose — bars, flexible edit window, menu bar, cursor")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    const tui_grid &g = m.compose(r, editor_tree(w, "one\ntwo\nthree", 4),
				  8, 40);
    REQUIRE(g.rows == 8u);
    // Row 0: the heading bar, reverse, content right-aligned (one blank
    // column of right margin).
    CHECK(g.row_text(0) == " notes.txt" + std::string(26, ' ') + "[+]");
    CHECK(g.at(0, 0).attr == tui_attr::reverse());
    CHECK(g.at(0, 39).attr == tui_attr::reverse());
    // Rows 1..5: the flexible edit window (8 - 3 fixed rows = 5).
    CHECK(g.row_text(1) == "one");
    CHECK(g.row_text(2) == "two");
    CHECK(g.row_text(3) == "three");
    CHECK(g.row_text(4) == "");
    // Row 6: the status bar; row 7: the menu with Save selected.
    CHECK(g.row_text(6) == " Ln 1");
    CHECK(g.at(6, 0).attr == tui_attr::reverse());
    CHECK(g.row_text(7) == " Save   Find   Quit");
    // Caret at byte 4 = line 2 col 0; the edit region starts at row 1.
    CHECK(g.cursor_visible);
    CHECK(g.cursor_row == 2u);
    CHECK(g.cursor_col == 0u);
    // Focus starts on the first focusable (the edit region), so the menu
    // selection is not highlighted as the cursor's home — but the
    // selected option still renders reverse.
    CHECK(g.at(7, 1).attr == tui_attr::reverse());	// " Save "
    CHECK(g.at(7, 9).attr == tui_attr::normal());		// " Find "
    REQUIRE(m.focusables().size() == 2u);
    CHECK(m.focusables()[0].k == tui_model::focusable::kind::edit);
    CHECK(m.focusables()[1].k == tui_model::focusable::kind::choice);
}

TEST_CASE("compose — edit window scrolls to keep the caret visible")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    // Ten lines, a 3-row window (6 rows - 3 fixed... use rows=6: 6-3=3).
    std::string doc = "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10";
    // Caret on l7 (byte offset of "l7" = 3*6 = 18).
    const tui_grid &g = m.compose(r, editor_tree(w, doc, 18), 6, 20);
    CHECK(g.row_text(1) == "l5");	// scrolled: window l5..l7
    CHECK(g.row_text(3) == "l7");
    CHECK(g.cursor_row == 3u);
    // Move the caret back to the top: the window follows.
    const tui_grid &g2 = m.compose(r, editor_tree(w, doc, 0), 6, 20);
    CHECK(g2.row_text(1) == "l1");
    CHECK(g2.cursor_row == 1u);
}

TEST_CASE("compose — long line shifts horizontally; selection highlights")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    std::string line(30, 'x');
    const tui_grid &g = m.compose(r, editor_tree(w, line, 30), 6, 20);
    // Caret at byte 30 on a 20-col grid: the region shifts left by 11.
    CHECK(g.cursor_row == 1u);
    CHECK(g.cursor_col == 19u);
    CHECK(g.row_text(1) == std::string(19, 'x'));

    // Selection bytes 4..9 of "one two three" render reverse.
    tui_model m2;
    const tui_grid &s = m2.compose(r, editor_tree(w, "one two three", 4,
						  4, 9), 6, 20);
    CHECK(s.at(1, 3).attr == tui_attr::normal());
    CHECK(s.at(1, 4).attr == tui_attr::reverse());
    CHECK(s.at(1, 8).attr == tui_attr::reverse());
    CHECK(s.at(1, 9).attr == tui_attr::normal());
}

TEST_CASE("compose — tabs expand to 8-column stops; the caret, shift and "
	  "selection convert through the display map (IDE-9c)")
{
    world w;
    roles r = roles::standard(w);

    // A tab is ONE byte but a run of display columns: "\tx" shows x at
    // column 8; "ab\tc" shows c at the next stop. Cells hold the expanded
    // spaces — a raw '\t' in a cell would move the terminal cursor WITHOUT
    // erasing the skipped columns (the scroll-corruption defect).
    tui_model m;
    const tui_grid &g = m.compose(r, editor_tree(w, "\tx\nab\tc", 1), 6, 40);
    CHECK(g.row_text(1) == std::string(8, ' ') + "x");
    CHECK(g.at(1, 0).ch == ' ');
    CHECK(g.at(1, 8).ch == 'x');
    CHECK(g.row_text(2) == "ab" + std::string(6, ' ') + "c");
    CHECK(g.at(2, 8).ch == 'c');
    // Caret at byte 1 of "\tx" (on the x) = display column 8.
    CHECK(g.cursor_row == 1u);
    CHECK(g.cursor_col == 8u);

    // Selection covering the tab byte highlights the WHOLE expanded run:
    // bytes 0..2 of "\tabc" = display columns 0..8 (tab) + 8 (the 'a').
    tui_model m2;
    const tui_grid &s = m2.compose(r, editor_tree(w, "\tabc", 0, 0, 2),
				   6, 40);
    CHECK(s.at(1, 0).attr == tui_attr::reverse());
    CHECK(s.at(1, 7).attr == tui_attr::reverse());
    CHECK(s.at(1, 8).attr == tui_attr::reverse());
    CHECK(s.at(1, 9).attr == tui_attr::normal());

    // The horizontal shift is display-column based: a caret at byte 21 of
    // a tab-headed 20-x line sits at display column 28 — on a 20-col grid
    // the window shifts by 9 and the caret lands on the last column.
    tui_model m3;
    const tui_grid &h = m3.compose(r, editor_tree(w, "\t" + std::string(20,
						  'x'), 21), 6, 20);
    CHECK(h.cursor_row == 1u);
    CHECK(h.cursor_col == 19u);
    CHECK(h.row_text(1) == std::string(19, 'x'));

    // THE CELL INVARIANT belt: a control byte reaching put() renders as a
    // visible '?', never as a raw byte the terminal would interpret.
    tui_grid raw;
    raw.resize(2, 10);
    raw.put(0, 0, std::string("a\x01") + "b\x7f" + "c");
    CHECK(raw.row_text(0) == "a?b?c");
}

TEST_CASE("compose — the tabwidth hint changes the stops (IDE-9d ^T option)")
{
    world w;
    roles r = roles::standard(w);

    // hints["tabwidth"] = 4: "\tx" shows x at column 4; "ab\tc" at the
    // next 4-stop; the caret converts through the same map.
    tui_model m;
    const tui_grid &g = m.compose(r, editor_tree(w, "\tx\nab\tc", 1,
						 -1, -1, 4), 6, 40);
    CHECK(g.row_text(1) == std::string(4, ' ') + "x");
    CHECK(g.at(1, 4).ch == 'x');
    CHECK(g.row_text(2) == "ab" + std::string(2, ' ') + "c");
    CHECK(g.at(2, 4).ch == 'c');
    CHECK(g.cursor_col == 4u);

    // Absent hint keeps the historical 8-stop; out-of-range clamps.
    tui_model m2;
    const tui_grid &d = m2.compose(r, editor_tree(w, "\tx", 1), 6, 40);
    CHECK(d.at(1, 8).ch == 'x');
    tui_model m3;
    const tui_grid &c = m3.compose(r, editor_tree(w, "\tx", 1, -1, -1, 99),
				   6, 40);
    CHECK(c.at(1, 16).ch == 'x');	// clamped to 16
}

// A bare edit node with a caret and optional rows/focus hints (IDE-9e
// windows: heights and focus are DATA the composer carries).
static uinode edit_node(world &w, const std::string &doc, long caret,
			long rows = 0, bool focus = false)
{
    roles r = roles::standard(w);
    uinode edit(r.edit);
    edit.content = madc::value(doc);
    std::map<std::string, madc::value> h;
    h["caret"] = madc::value((int64_t)caret);
    if ( rows > 0 )
	h["rows"] = madc::value((int64_t)rows);
    if ( focus )
	h["focus"] = madc::value((int64_t)1);
    edit.hints = madc::value::make_object(h);
    return edit;
}

static uinode status_node(world &w, const char *text)
{
    roles r = roles::standard(w);
    uinode status(r.status);
    status.content = madc::value(std::string(text));
    return status;
}

TEST_CASE("compose — the rows hint partitions edit windows (IDE-9e); the "
	  "focus hint picks the cursor's edit; unhinted prompts unchanged")
{
    world w;
    roles r = roles::standard(w);

    // Two windows, JOE shape: status + edit per window. 10 rows total:
    // status(1) + top edit rows:4 + status(1) + bottom edit rows:4.
    // The bottom window carries the focus hint — the grid cursor lands
    // in ITS region even though the top edit is the earlier focusable.
    uinode root(r.group);
    root.add(status_node(w, "top.mad"));
    root.add(edit_node(w, "a1\na2\na3", 0, 4));
    root.add(status_node(w, "bot.mad"));
    root.add(edit_node(w, "b1\nb2\nb3", 3, 4, true));
    tui_model m;
    const tui_grid &g = m.compose(r, root, 10, 20);
    CHECK(g.row_text(0) == " top.mad");		// window 1 status
    CHECK(g.row_text(1) == "a1");			// window 1 edit rows 1..4
    CHECK(g.row_text(3) == "a3");
    CHECK(g.row_text(4) == "");
    CHECK(g.row_text(5) == " bot.mad");		// window 2 status
    CHECK(g.row_text(6) == "b1");			// window 2 edit rows 6..9
    CHECK(g.row_text(7) == "b2");
    // Focus hint: the cursor paints in the SECOND window (caret 3 = b2
    // col 0 = grid row 7), and its slot is the model focus.
    CHECK(g.cursor_visible);
    CHECK(g.cursor_row == 7u);
    CHECK(g.cursor_col == 0u);
    CHECK(m.focus_slot() == 1u);
    // Per-slot scroll: each window keeps its own caret visible without
    // disturbing the other (move window 2's caret far down a longer doc).
    uinode root2(r.group);
    root2.add(status_node(w, "top.mad"));
    root2.add(edit_node(w, "a1\na2\na3", 0, 4));
    root2.add(status_node(w, "bot.mad"));
    root2.add(edit_node(w, "b1\nb2\nb3\nb4\nb5\nb6\nb7", 18, 4, true));
    const tui_grid &g2 = m.compose(r, root2, 10, 20);
    CHECK(g2.row_text(1) == "a1");			// window 1 unmoved
    CHECK(g2.row_text(9) == "b7");			// window 2 scrolled
    CHECK(g2.cursor_row == 9u);

    // Negative control: the unhinted prompt shape is byte-identical to
    // the legacy rule — first unhinted flexible, later unhinted 1 row.
    uinode p(r.group);
    p.add(edit_node(w, "body\ntext", 0));
    p.add(status_node(w, "St"));
    p.add(edit_node(w, "prompt-line", 0));
    tui_model mp;
    const tui_grid &gp = mp.compose(r, p, 6, 20);
    CHECK(gp.row_text(0) == "body");		// flexible: rows 0..3
    CHECK(gp.row_text(1) == "text");
    CHECK(gp.row_text(4) == " St");
    CHECK(gp.row_text(5) == "prompt-line");	// one row
}

// One span row { s, e, c } for the hints["spans"] array.
static madc::value span_row(long s, long e, const char *colour)
{
    std::map<std::string, madc::value> f;
    f["s"] = madc::value((int64_t)s);
    f["e"] = madc::value((int64_t)e);
    f["c"] = madc::value(std::string(colour));
    return madc::value::make_object(f);
}

TEST_CASE("styles — the JOE-vocabulary spec parser (one table)")
{
    tui_attr a;
    REQUIRE(tui_attr_of("yellow", a));
    CHECK(a.fg == 4);				// black..white = 1..8
    CHECK(a.bg == 0);
    CHECK(a.flags == 0);
    REQUIRE(tui_attr_of("bold yellow", a));	// bold-as-bright: the 16
    CHECK(a.fg == 4);
    CHECK((a.flags & tui_attr::BOLD) != 0);
    REQUIRE(tui_attr_of("underline bg_blue cyan", a));
    CHECK(a.fg == 7);
    CHECK(a.bg == 5);
    CHECK((a.flags & tui_attr::UNDERLINE) != 0);
    REQUIRE(tui_attr_of("inverse", a));
    CHECK(a.is_reverse());
    REQUIRE(tui_attr_of("reverse", a));		// JOE synonym
    CHECK(a.is_reverse());
    REQUIRE(tui_attr_of("normal", a));
    CHECK(a.is_normal());
    CHECK(!tui_attr_of("mauve", a));		// unknown word refuses
    CHECK(!tui_attr_of("bold mauve", a));	// ... the WHOLE spec
    CHECK(!tui_attr_of("", a));			// empty refuses
}

TEST_CASE("compose — highlight spans paint; the selection wins; bad rows skip")
{
    tui_attr yellow, cyan;
    REQUIRE(tui_attr_of("yellow", yellow));
    REQUIRE(tui_attr_of("bold cyan", cyan));

    world w;
    roles r = roles::standard(w);
    uinode root(r.group);
    uinode edit(r.edit);
    edit.content = madc::value(std::string("int n = 42; // c"));
    std::map<std::string, madc::value> h;
    h["caret"] = madc::value((int64_t)0);
    h["sel_start"] = madc::value((int64_t)8);
    h["sel_end"] = madc::value((int64_t)10);
    std::vector<madc::value> rows;
    rows.push_back(span_row(0, 3, "yellow"));	// "int"
    rows.push_back(span_row(8, 10, "green"));	// "42" — under the selection
    rows.push_back(span_row(12, 16, "bold cyan"));	// "// c"
    rows.push_back(span_row(5, 2, "red"));	// end <= start: skipped
    rows.push_back(span_row(4, 6, "mauve"));	// unknown colour: skipped
    h["spans"] = madc::value::make_array(rows);
    edit.hints = madc::value::make_object(h);
    root.add(edit);
    tui_model m;
    const tui_grid &g = m.compose(r, root, 4, 40);
    CHECK(g.row_text(0) == "int n = 42; // c");
    CHECK(g.at(0, 0).attr == yellow);
    CHECK(g.at(0, 2).attr == yellow);
    CHECK(g.at(0, 3).attr == tui_attr::normal());	// the space after "int"
    CHECK(g.at(0, 4).attr == tui_attr::normal());	// both bad rows skipped
    CHECK(g.at(0, 8).attr == tui_attr::reverse());	// selection WINS over green
    CHECK(g.at(0, 9).attr == tui_attr::reverse());
    CHECK(g.at(0, 12).attr == cyan);
    CHECK(g.at(0, 15).attr == cyan);
}

TEST_CASE("events — coalescing, focus cycle, choice navigation, choose")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    m.compose(r, editor_tree(w, "abc", 0), 8, 40);

    // A printable run coalesces into ONE text event; the arrow that
    // follows arrives separately (§7.5).
    std::vector<tui_keyev> keys;
    tui_keyparse p;
    p.feed("hello\x1b[D", 8, keys);
    std::vector<tui_event> ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::text);
    CHECK(ev[0].text == "hello");
    CHECK(ev[1].kind == tui_event_kind::key);
    CHECK(ev[1].key == tui_key::left);	// edit focused: app owns the caret
    CHECK(!ev[1].choice_focused);	// no focused choice: no row rides

    // Tab cycles focus onto the menu; arrows now navigate it (selection
    // is presentation state — the event only says "repaint").
    keys.clear();
    keys.push_back(tui_keyev(tui_key::tab));
    keys.push_back(tui_keyev(tui_key::right));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::focus);
    CHECK(m.focus_slot() == 1u);
    CHECK(m.selection_of(1) == 1u);	// Save -> Find

    // Enter on the focused choice CHOOSES: the option's action id rides
    // the event — the same option line mode would number 2.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::enter));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::choose);
    CHECK(ev[0].option == 1u);
    CHECK(ev[0].action == w.intern("f"));

    // Selection wraps in both directions.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::down));
    keys.push_back(tui_keyev(tui_key::down));
    m.apply_keys(keys);
    CHECK(m.selection_of(1) == 0u);	// Find -> Quit -> wrap to Save
    keys.clear();
    keys.push_back(tui_keyev(tui_key::up));
    m.apply_keys(keys);
    CHECK(m.selection_of(1) == 2u);	// wrap back to Quit

    // A key the widget does not consume (del/ins) reaches the application
    // WITH the focused choice's live selection riding along — the app can
    // act on the focused row while selection stays presentation state.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::del));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::key);
    CHECK(ev[0].key == tui_key::del);
    CHECK(ev[0].choice_focused);
    CHECK(ev[0].option == 2u);		// the wrapped-to Quit row

    // The selected option's highlight follows on the next compose.
    const tui_grid &g = m.compose(r, editor_tree(w, "abc", 0), 8, 40);
    CHECK(g.at(7, 1).attr == tui_attr::normal());		// " Save "
    CHECK(g.at(7, 15).attr == tui_attr::reverse());	// " Quit "
    // The menu holds focus, so no edit caret cursor shows.
    CHECK(!g.cursor_visible);

    // Ctrl chords and resize pass through as semantic events.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 's'));
    keys.push_back(tui_keyev(tui_key::resize));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::key);
    CHECK(ev[0].key == tui_key::ctrl);
    CHECK(ev[0].ch == 's');
    CHECK(ev[1].kind == tui_event_kind::resize);
}

// ---- the LIST presentation + autofocus (IDE-10a palettes): one choice
// focusable, two renderings — hints {list:1} = label row + ONE OPTION PER
// ROW (selected row reversed), {focus:1} = the slot takes focus at compose
// so arrows/enter navigate it with NO tab cycle (modal while up).

static uinode palette_tree(world &w)
{
    roles r = roles::standard(w);
    uinode root(r.group);
    uinode edit(r.edit);
    edit.content = madc::value(std::string("body"));
    root.add(edit);
    uinode pal(r.choice);
    pal.label = madc::value(std::string("File: a_"));
    pal.add(option(w, "* a.mad", "pal-open"));
    pal.add(option(w, "  b.c", "pal-open"));
    pal.add(option(w, "  c.h", "pal-open"));
    std::map<std::string, madc::value> h;
    h["list"] = madc::value((int64_t)1);
    h["focus"] = madc::value((int64_t)1);
    pal.hints = madc::value::make_object(h);
    root.add(pal);
    return root;
}

TEST_CASE("compose — list choice: label row, one option per row, autofocus")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    const tui_grid &g = m.compose(r, palette_tree(w), 8, 40);
    // 4 fixed rows (label + 3 options) follow the flexible edit window.
    CHECK(g.row_text(4) == "File: a_");
    CHECK(g.row_text(5) == "  * a.mad");
    CHECK(g.row_text(6) == "    b.c");
    CHECK(g.row_text(7) == "    c.h");
    // The selected row (0) renders reverse across its text; the others
    // stay normal.
    CHECK(g.at(5, 0).attr == tui_attr::reverse());
    CHECK(g.at(5, 8).attr == tui_attr::reverse());
    CHECK(g.at(6, 0).attr == tui_attr::normal());
    // Autofocus: the choice slot holds focus straight from compose.
    REQUIRE(m.focusables().size() == 2u);
    CHECK(m.focus_slot() == 1u);
}

TEST_CASE("events — autofocused list choice: arrows/enter with no tab; filter text still coalesces")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    m.compose(r, palette_tree(w), 8, 40);
    // Down moves the selection immediately — no tab cycle first.
    std::vector<tui_keyev> keys;
    keys.push_back(tui_keyev(tui_key::down));
    std::vector<tui_event> ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(m.selection_of(1) == 1u);
    // Printables never navigate: the filter run reaches the app as ONE
    // text event even while the choice holds focus.
    keys.clear();
    tui_keyparse p;
    p.feed("ab", 2, keys);
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::text);
    CHECK(ev[0].text == "ab");
    // Enter chooses the selected row, carrying its action.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::enter));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::choose);
    CHECK(ev[0].option == 1u);
    CHECK(ev[0].action == w.intern("pal-open"));
}

TEST_CASE("dirty rows — only changed rows repaint; a resize dirties all")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    tui_grid prev = m.compose(r, editor_tree(w, "one\ntwo", 0), 6, 20);
    const tui_grid &next = m.compose(r, editor_tree(w, "one\ntwX", 7), 6, 20);
    std::vector<size_t> dirty = tui_dirty_rows(prev, next);
    // Line 2's text changed AND the caret moved lines (no cell change on
    // row 1 — the cursor is grid metadata, not a cell).
    REQUIRE(dirty.size() == 1u);
    CHECK(dirty[0] == 2u);

    tui_grid small;
    small.resize(3, 20);
    CHECK(tui_dirty_rows(small, next).size() == next.rows);
}

// ---- the diff PLAN (IDE-9c scroll feel): a pure vertical shift becomes a
// terminal scroll op + entering-row repaints instead of a full repaint.

static tui_grid plan_grid(const std::vector<std::string> &rows, size_t cols)
{
    tui_grid g;
    g.resize(rows.size(), cols);
    for ( size_t r = 0; r < rows.size(); ++r )
	g.put(r, 0, rows[r]);
    return g;
}

TEST_CASE("diff plan — a one-line scroll shifts; chrome + entering rows repaint")
{
    std::vector<std::string> a, b;
    a.push_back("status ONE");
    for ( int i = 1; i <= 9; ++i )
	a.push_back("line " + std::to_string(i)
		    + " with enough text to matter");
    b.push_back("status TWO");			// chrome changes every step
    for ( int i = 2; i <= 9; ++i )
	b.push_back("line " + std::to_string(i)
		    + " with enough text to matter");
    b.push_back("line 10 entering");
    tui_grid prev = plan_grid(a, 40), next = plan_grid(b, 40);
    // the status row is INVERSE-filled chrome — outside any shift band
    prev.fill_attr(0, 0, 40, tui_attr::reverse());
    next.fill_attr(0, 0, 40, tui_attr::reverse());

    tui_paint_plan p = tui_diff_plan(prev, next);
    REQUIRE(p.shifted);
    CHECK(p.up);
    CHECK(p.delta == 1u);
    CHECK(p.top == 1u);
    CHECK(p.bot == 9u);
    REQUIRE(p.spans.size() == 2u);	// vs 10 dirty rows unplanned
    CHECK(p.spans[0].row == 0u);	// the status row: only "ONE"->"TWO"
    CHECK(p.spans[0].c0 == 7u);
    CHECK(p.spans[0].c1 == 9u);
    CHECK(p.spans[1].row == 9u);	// the entering row, its content
    CHECK(p.spans[1].c0 == 0u);
    CHECK(p.spans[1].c1 == 15u);
}

TEST_CASE("diff plan — scroll down (RI shape) inserts at the top")
{
    std::vector<std::string> a, b;
    a.push_back("status");
    for ( int i = 5; i <= 13; ++i )
	a.push_back("line " + std::to_string(i)
		    + " with enough text to matter");
    b.push_back("status");			// chrome unchanged this time
    b.push_back("line 4 entering");
    for ( int i = 5; i <= 12; ++i )
	b.push_back("line " + std::to_string(i)
		    + " with enough text to matter");
    tui_grid prev = plan_grid(a, 40), next = plan_grid(b, 40);

    tui_paint_plan p = tui_diff_plan(prev, next);
    REQUIRE(p.shifted);
    CHECK(!p.up);
    CHECK(p.delta == 1u);
    CHECK(p.top == 1u);
    CHECK(p.bot == 9u);
    REQUIRE(p.spans.size() == 1u);
    CHECK(p.spans[0].row == 1u);	// only the entering row repaints
}

TEST_CASE("diff plan — a page jump shifts by the page delta")
{
    // Genuinely distinct line bodies — lines differing only in a digit
    // make the plain span diff (one-cell spans) cheaper than scrolling,
    // and the cost model rightly refuses the shift for those.
    std::vector<std::string> a, b;
    a.push_back("status");
    for ( int i = 1; i <= 9; ++i )
	a.push_back("line " + std::to_string(i) + " "
		    + std::string((size_t)i, '#'));
    b.push_back("status");
    for ( int i = 6; i <= 14; ++i )
	b.push_back("line " + std::to_string(i) + " "
		    + std::string((size_t)i, '#'));
    tui_grid prev = plan_grid(a, 40), next = plan_grid(b, 40);

    tui_paint_plan p = tui_diff_plan(prev, next);
    REQUIRE(p.shifted);
    CHECK(p.up);
    CHECK(p.delta == 5u);
    CHECK(p.top == 1u);
    CHECK(p.bot == 9u);
    CHECK(p.spans.size() == 5u);	// rows 5..9 enter; vs 9 dirty
}

TEST_CASE("diff plan — scattered edits and small diffs stay plain")
{
    std::vector<std::string> a;
    a.push_back("status");
    for ( int i = 1; i <= 9; ++i )
	a.push_back("line " + std::to_string(i)
		    + " with enough text to matter");
    tui_grid prev = plan_grid(a, 40);

    std::vector<std::string> b = a;	// scattered content edits, no shift
    b[2] = "edited AA";
    b[4] = "edited BB";
    b[6] = "edited CC";
    b[8] = "edited DD";
    tui_grid next = plan_grid(b, 40);
    tui_paint_plan p = tui_diff_plan(prev, next);
    CHECK(!p.shifted);
    CHECK(p.spans.size() == 4u);

    // a 2-row diff sits below the detection threshold: stays plain
    tui_grid small_next = plan_grid(a, 40);
    small_next.put(3, 0, "edited                                  ");
    small_next.put(7, 0, "edited                                  ");
    tui_paint_plan q = tui_diff_plan(prev, small_next);
    CHECK(!q.shifted);
    CHECK(q.spans.size() == 2u);
}

TEST_CASE("diff plan — a dimension change is a plain full repaint")
{
    std::vector<std::string> a(10, std::string("row"));
    tui_grid prev = plan_grid(a, 40);
    std::vector<std::string> b(12, std::string("row"));
    tui_grid next = plan_grid(b, 40);
    tui_paint_plan p = tui_diff_plan(prev, next);
    CHECK(!p.shifted);
    CHECK(p.spans.size() == next.rows);
}

TEST_CASE("row_paint_end — the EL boundary: normal-space tails only")
{
    tui_grid g;
    g.resize(3, 20);
    g.put(0, 0, "abc");			// normal-space tail
    CHECK(g.row_paint_end(0) == 3u);
    CHECK(g.row_paint_end(1) == 0u);	// blank row: EL does it all
    g.put(2, 0, "st");			// an inverse status fill is NOT
    g.fill_attr(2, 0, 20, tui_attr::reverse());	// erasable by EL
    CHECK(g.row_paint_end(2) == 20u);
    g.put(1, 19, "x");			// content in the last column
    CHECK(g.row_paint_end(1) == 20u);
    CHECK(g.row_paint_end(99) == 0u);	// out of range clips
}

// ---- the bindings-as-data chord adapter (madcide IDE-1; owner-directed
// JOE/WordStar ^K chords, configurable — a table of key sequences to
// action names, never a second hardcoded map).

TEST_CASE("key spelling — one owner, both directions")
{
    CHECK(tui_key_name(tui_keyev(tui_key::ctrl, 'k')) == "^k");
    CHECK(tui_key_name(tui_keyev(tui_key::ch, 's')) == "s");
    CHECK(tui_key_name(tui_keyev(tui_key::ch, ' ')) == "space");
    CHECK(tui_key_name(tui_keyev(tui_key::pgup)) == "pgup");

    tui_keyev k;
    REQUIRE(tui_key_from_name("^s", k));
    CHECK(k.kind == tui_key::ctrl);
    CHECK(k.ch == 's');
    REQUIRE(tui_key_from_name("^S", k));	// generous in, canonical out
    CHECK(k.ch == 's');
    REQUIRE(tui_key_from_name("q", k));
    CHECK(k.kind == tui_key::ch);
    CHECK(k.ch == 'q');
    REQUIRE(tui_key_from_name("space", k));
    CHECK(k.kind == tui_key::ch);
    CHECK(k.ch == ' ');
    REQUIRE(tui_key_from_name("home", k));
    CHECK(k.kind == tui_key::home);
    CHECK(!tui_key_from_name("", k));
    CHECK(!tui_key_from_name("^!", k));
    CHECK(!tui_key_from_name("nosuch", k));

    // Punctuation controls round-trip like letters do.
    CHECK(tui_key_name(tui_keyev(tui_key::ctrl, '_')) == "^_");
    CHECK(tui_key_name(tui_keyev(tui_key::ctrl, '^')) == "^^");
    REQUIRE(tui_key_from_name("^_", k));
    CHECK(k.kind == tui_key::ctrl);
    CHECK(k.ch == '_');
    REQUIRE(tui_key_from_name("^^", k));
    CHECK(k.ch == '^');
    REQUIRE(tui_key_from_name("^\\", k));
    CHECK(k.ch == '\\');
    REQUIRE(tui_key_from_name("^]", k));
    CHECK(k.ch == ']');
}

TEST_CASE("bindings — build validation is loud and whole-table")
{
    tui_bindings b;
    CHECK(!b.bind("^k nosuchkey", "x"));	// unknown spelling
    CHECK(!b.bind("", "x"));
    CHECK(b.bind("^K S", "save"));		// normalizes to "^k s"
    CHECK(b.bind("^k q", "quit"));
    std::string err;
    CHECK(b.finalize(err));
    CHECK(b.bound("^k s"));
    CHECK(b.action_of("^k s") == "save");
    CHECK(b.prefix("^k"));
    CHECK(!b.bound("^k"));

    tui_bindings head;				// printable-headed: refused
    CHECK(head.bind("g g", "goto"));
    CHECK(!head.finalize(err));
    CHECK(err.find("printable-headed") != std::string::npos);

    tui_bindings shadow;			// prefix conflict: refused
    CHECK(shadow.bind("^k", "block"));
    CHECK(shadow.bind("^k s", "save"));
    CHECK(!shadow.finalize(err));
    CHECK(err.find("shadows") != std::string::npos);

    // JOE's OTHER chord convention: a ctrl+letter CONTINUATION is the
    // letter (^K ^Z == ^K Z — users keep ctrl held). Both spellings
    // canonicalize to one slot; ctrl+punctuation continuations keep
    // their ctrl form (^K ^_ is not ^K _).
    tui_bindings ctrlcont;
    CHECK(ctrlcont.bind("^k ^z", "shell"));
    CHECK(ctrlcont.bind("^k ^_", "special"));
    CHECK(ctrlcont.finalize(err));
    CHECK(ctrlcont.bound("^k z"));
    CHECK(ctrlcont.action_of("^k z") == "shell");
    CHECK(ctrlcont.bound("^k ^_"));
    CHECK(!ctrlcont.bound("^k _"));
    CHECK(tui_bindings::cont_spelling(tui_keyev(tui_key::ctrl, 'z')) == "z");
    CHECK(tui_bindings::cont_spelling(tui_keyev(tui_key::ctrl, '_')) == "^_");
    CHECK(tui_bindings::cont_spelling(tui_keyev(tui_key::ch, 'Z')) == "z");
}

TEST_CASE("chords — a ctrl-held continuation completes the chord (JOE)")
{
    tui_bindings b;
    b.bind("^k z", "shell");
    std::string err;
    REQUIRE(b.finalize(err));
    tui_model m;
    m.set_bindings(b);
    std::vector<tui_keyev> keys;
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ctrl, 'z'));	// ctrl still held
    std::vector<tui_event> ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);	// chord opened: repaint (%k)
    CHECK(ev[1].kind == tui_event_kind::action);
    CHECK(ev[1].action_name == "shell");
    CHECK(ev[1].seq == "^k z");

    // A three-key chord repaints on the open AND on each extension —
    // the %k echo grows live ("^k", then "^k e") before the action.
    tui_bindings b3;
    b3.bind("^k e c", "deep");
    REQUIRE(b3.finalize(err));
    tui_model m3;
    m3.set_bindings(b3);
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 'e'));
    keys.push_back(tui_keyev(tui_key::ch, 'c'));
    ev = m3.apply_keys(keys);
    REQUIRE(ev.size() == 3u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::focus);
    CHECK(ev[2].kind == tui_event_kind::action);
    CHECK(ev[2].action_name == "deep");
    CHECK(ev[2].seq == "^k e c");
}

static tui_bindings joe_table()
{
    tui_bindings b;
    b.bind("^k s", "save");
    b.bind("^k q", "quit");
    b.bind("^s", "search");	// a single-key binding rides the same table
    std::string err;
    REQUIRE(b.finalize(err));
    return b;
}

TEST_CASE("chords — resolve, coalesce around, miss, cancel, persist")
{
    tui_model m;
    m.set_bindings(joe_table());

    // A printable run flushes BEFORE the chord fires; the chord OPENING
    // emits a focus (repaint) event — the pending prefix is visible
    // state (JOE's %k echo); the chord's own printable continuation
    // never joins a text run.
    std::vector<tui_keyev> keys;
    keys.push_back(tui_keyev(tui_key::ch, 'a'));
    keys.push_back(tui_keyev(tui_key::ch, 'b'));
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 's'));
    keys.push_back(tui_keyev(tui_key::ch, 'c'));
    std::vector<tui_event> ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 4u);
    CHECK(ev[0].kind == tui_event_kind::text);
    CHECK(ev[0].text == "ab");
    CHECK(ev[1].kind == tui_event_kind::focus);
    CHECK(ev[2].kind == tui_event_kind::action);
    CHECK(ev[2].action_name == "save");
    CHECK(ev[2].seq == "^k s");
    CHECK(ev[3].kind == tui_event_kind::text);
    CHECK(ev[3].text == "c");

    // Chord continuations are letter-case-insensitive (JOE's ^K S == ^K s):
    // a shifted continuation matches the same binding.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 'S'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::action);
    CHECK(ev[1].action_name == "save");
    CHECK(ev[1].seq == "^k s");

    // Single-key binding fires directly.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 's'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "search");
    CHECK(ev[0].seq == "^s");

    // An unbound completion reports the miss: empty action, the seq.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 'z'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::action);
    CHECK(ev[1].action_name == "");
    CHECK(ev[1].seq == "^k z");

    // esc cancels a pending chord — the cancel repaints too (the %k
    // echo must clear); the next key is ordinary.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::esc));
    keys.push_back(tui_keyev(tui_key::ch, 'x'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 3u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::focus);
    CHECK(m.pending_chord() == "");
    CHECK(ev[2].kind == tui_event_kind::text);
    CHECK(ev[2].text == "x");

    // A resize passes through mid-chord and the chord still completes —
    // across apply_keys BATCHES (pending is adapter state, and the
    // start-focus event leaves it readable for the %k seat).
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::resize));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::resize);
    CHECK(m.pending_chord() == "^k");
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ch, 'q'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "quit");

    // A wake (stage-2: cooperative tasks drained) has resize's exact
    // transparency: it passes through mid-chord without disturbing the
    // pending prefix, and the chord still completes.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::wake));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::wake);
    CHECK(m.pending_chord() == "^k");
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ch, 'q'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "quit");
    // Outside a chord: one wake in, one wake event out.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::wake));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::wake);
}

TEST_CASE("chords — bindings win over navigation; a swap restores it")
{
    world w;
    roles r = roles::standard(w);
    tui_model m;
    m.compose(r, editor_tree(w, "text", 0), 8, 40);
    // Focus the menu, then bind `up` — the profile owns the key now.
    std::vector<tui_keyev> keys;
    keys.push_back(tui_keyev(tui_key::tab));
    std::vector<tui_event> ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);

    tui_bindings b;
    b.bind("up", "previous");
    std::string err;
    REQUIRE(b.finalize(err));
    m.set_bindings(b);
    keys.clear();
    keys.push_back(tui_keyev(tui_key::up));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "previous");

    // Swapping to an EMPTY table restores the built-in interpretation
    // (choice navigation) — and abandons any pending chord.
    m.set_bindings(tui_bindings());
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::focus);
}
