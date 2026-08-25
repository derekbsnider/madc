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
			  long sel_start = -1, long sel_end = -1)
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
    CHECK(g.at(0, 0).attr == tui_attr::reverse);
    CHECK(g.at(0, 39).attr == tui_attr::reverse);
    // Rows 1..5: the flexible edit window (8 - 3 fixed rows = 5).
    CHECK(g.row_text(1) == "one");
    CHECK(g.row_text(2) == "two");
    CHECK(g.row_text(3) == "three");
    CHECK(g.row_text(4) == "");
    // Row 6: the status bar; row 7: the menu with Save selected.
    CHECK(g.row_text(6) == " Ln 1");
    CHECK(g.at(6, 0).attr == tui_attr::reverse);
    CHECK(g.row_text(7) == " Save   Find   Quit");
    // Caret at byte 4 = line 2 col 0; the edit region starts at row 1.
    CHECK(g.cursor_visible);
    CHECK(g.cursor_row == 2u);
    CHECK(g.cursor_col == 0u);
    // Focus starts on the first focusable (the edit region), so the menu
    // selection is not highlighted as the cursor's home — but the
    // selected option still renders reverse.
    CHECK(g.at(7, 1).attr == tui_attr::reverse);	// " Save "
    CHECK(g.at(7, 9).attr == tui_attr::normal);		// " Find "
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
    CHECK(s.at(1, 3).attr == tui_attr::normal);
    CHECK(s.at(1, 4).attr == tui_attr::reverse);
    CHECK(s.at(1, 8).attr == tui_attr::reverse);
    CHECK(s.at(1, 9).attr == tui_attr::normal);
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

    // The selected option's highlight follows on the next compose.
    const tui_grid &g = m.compose(r, editor_tree(w, "abc", 0), 8, 40);
    CHECK(g.at(7, 1).attr == tui_attr::normal);		// " Save "
    CHECK(g.at(7, 15).attr == tui_attr::reverse);	// " Quit "
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
