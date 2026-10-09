#ifndef __MADCDIS_TUI_MODEL_H
#define __MADCDIS_TUI_MODEL_H 1

// madcdis/tui_model.h — the level-1 renderer's MODEL (Track 7.2 R5):
// everything an addressable-character-grid frontend does that is not
// terminal I/O, dependency-free and unit-testable. The uinode tree in,
// a cell grid + semantic events out:
//
//   - layout: the same semantic tree the level-0 renderer linearizes,
//     composed onto a rows×cols grid (heading/status bars, wrapped
//     content, a flexible `edit` window, a `choice` menu bar);
//   - focus and selection: choice options are NAVIGABLE (the same tree
//     line mode numbers — design success criterion 4); focus cycles across
//     choice/edit nodes — compose DISCOVERS the focusables here, the focus
//     slot, the per-choice selection and the tab/arrow/enter rules are the
//     shared owner's (madcdis/ui_focus.h focus_state), consumed by this
//     model and the DOM model alike;
//   - the input adapter: raw terminal bytes → keys (tui_keyparse, in
//     madcdis/tui_keyparse.h: CSI/SS3 escape parsing with an explicit
//     flush for the bare-ESC pause) and
//     keys → SEMANTIC events, coalescing printable runs into one text
//     event (design §7.5 — five key events never become five domain
//     transactions); the key VOCABULARY, its spelling, the bindings
//     table and the chord resolver are the shared key owner in
//     madcdis/keys.h — this model consumes it (key_resolver::step);
//   - differential support: dirty-row comparison between two grids
//     (the grid and its diff: madcdis/tui_grid.h).
//
// The TARGET (a provider behind the ui:: session surface — the hand-
// rolled VT100/xterm one in src/ui_term.cpp, owner-decided 2026-08-25
// over vendoring ncurses/termbox2/notcurses) only moves bytes: raw mode,
// escape emission, key reads. Richer providers plug in behind the same
// seam without touching this model.
//
// PRESENTATION STATE LIVES HERE (design §7.2): scroll windows, horizontal
// shift, focus slot, menu selection. Interaction state (caret, selection,
// search) is the APPLICATION's, carried on the tree as `edit`-node hints
// (byte offsets: "caret", "sel_start", "sel_end"); domain state never
// enters. Byte-oriented in this pilot: multi-byte (UTF-8) glyphs occupy
// one cell per byte — a named residue, not a contract.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a tui_model is
// a plain object confined to the thread that composes and applies keys;
// two models never share state.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "madcdis/uinode.h"
#include "madcdis/render_text.h"	// wrap_text — the one wrap owner
#include "madcdis/keys.h"		// tui_key, spelling, tui_bindings, key_resolver
#include "madcdis/ui_events.h"	// tui_event_kind, tui_event
#include "madcdis/ui_focus.h"	// focusable, focus_state — the focus/navigation owner
#include "madcdis/ui_input.h"	// ui_apply_keys — the one keys → events adapter
#include "madcdis/ui_style.h"	// ui_style, ui_style_of — the one render style + spec parser
#include "madcdis/text_utf16.h"	// line_layout / line_columns — the one line layout (B87)
#include "madcdis/tui_grid.h"	// tui_cell, tui_grid, the repaint diff (S0 split)
#include "madcdis/tui_frame.h"	// tui_frame — dividers and junctions by arms (S2)
#include "madcdis/tui_keyparse.h"	// raw bytes -> keys (S0 split)

namespace madc {
namespace hub {

// The chrome a theme colours (facelift S2): the dividers, the editor's
// line-number gutter, the caret line's number, and the caret line itself.
// The composer hands their style specs over as the root's `chrome` hint
// object (keys = these names); tui_chrome_of converts each key ONCE.
enum class tui_chrome : unsigned char
{
    divider = 0, gutter, gutter_current, current_line,
    tab, tab_active, statusbar,		// S3: strips and the status bar
    menubar, menu, menu_selected, menu_hot, shadow,	// S4: the menu bar
    toolbar,				// S5: the toolbar row
    dialog, list_selected, field, button_primary,	// S6: floating windows
    count
};
inline bool tui_chrome_of(const std::string &name, tui_chrome &out)
{
    static const char *const names[] = {
	"divider", "gutter", "gutter_current", "current_line",
	"tab", "tab_active", "statusbar",
	"menubar", "menu", "menu_selected", "menu_hot", "shadow",
	"toolbar", "dialog", "list_selected", "field", "button_primary"
    };
    for ( size_t i = 0; i < (size_t)tui_chrome::count; ++i )
	if ( name == names[i] )
	{
	    out = (tui_chrome)i;
	    return true;
	}
    return false;
}

// ------------------------------------------------------------------ the model
// One instance per TUI session. Contract: compose() before apply_keys()
// (events are interpreted against the focusables the last compose
// discovered), recompose after any focus/resize/text event. Focusable
// identity is discovery order — stable while the application composes
// the same tree shape, which is the Phase-1 contract.
class tui_model
{
public:
    // The focusable vocabulary is the shared focus owner's (madcdis/
    // ui_focus.h); the model's spelling stays for its consumers.
    typedef madc::hub::focusable focusable;

private:
    tui_grid _grid;
    tui_frame _frame;				// this compose's dividers (S2)
    ui_style _chrome[(size_t)tui_chrome::count];	// this compose's chrome styles
    focus_state _focus_st;			// focus slot + per-choice selection + navigation
    std::map<size_t, size_t> _scroll;		// per edit slot: top line
    std::map<size_t, size_t> _hshift;		// per edit slot: left shift
    key_resolver _keys;			// the ONE chord/key owner (madcdis/keys.h)
    size_t _tb_row;				// the toolbar's row (npos = none) and
    std::map<std::string, size_t> _tb_drops;	// each drop button's column, by
						// the menu it drops (S5)

    // One composed output line: text plus attribute spans.
    struct span { size_t col, len; ui_style attr; };
    struct line_out
    {
	std::string text;
	std::vector<span> spans;
	line_out() {}
	explicit line_out(const std::string &t) : text(t) {}
    };
    // A flexible edit region parked between fixed lines.
    // A document byte-range with a render style (AST-2 highlight spans):
    // parsed from the edit node's hints["spans"] rows { s, e, c } —
    // byte offsets + a colour NAME (ui_style_of converts at the
    // boundary; a malformed row is skipped — spans are presentation).
    struct doc_span
    {
	long start, end;
	ui_style attr;
	doc_span() : start(0), end(0), attr(ui_style::normal()) {}
    };
    struct edit_slot
    {
	size_t line_index;	// position in the fixed-line stream
	size_t slot;		// focusable index
	std::string text;	// the bound document text
	long caret;		// byte offsets from the node's hints
	long sel_start, sel_end;
	long tabw;		// display tab width (hints["tabwidth"], the
				// ^T option); clamped 1..16 at parse
	long rows;		// fixed height (hints["rows"], IDE-9e
				// windows — the composer owns the split
				// math and carries it as DATA); 0 = the
				// legacy rule (first unhinted flexible,
				// other unhinted one row — prompts)
	std::vector<doc_span> spans;	// highlight spans (may be empty)
	bool gutter;		// line numbers + the caret line (hints["gutter"],
				// the layout's pane flag)
	edit_slot() : line_index(0), slot(0), caret(0),
		      sel_start(-1), sel_end(-1), tabw(tab_stop),
		      rows(0), gutter(false) {}
    };

    // ---------------------------------------------------------- the layout tree
    // Rectangular compose (client-server arc V2): the root's children split
    // into CHROME (a node hinted region:sidebar|panel, carved as a band by its
    // side/size) and the centre FLOW (everything else, tree order -- JOE's
    // shape). A group hinted `split` inside the flow divides its rect; a leaf
    // pane's `tabs` render as its header line. No chrome shown and no split =
    // the linear stream, BYTE-IDENTICAL (the negative control). Collection
    // walks the tree ONCE in tree order (the focusable slot identity) wrapping
    // content to each region's final WIDTH; painting decides the rows.
    struct flow_item
    {
	enum class kind : unsigned char { line, edit, sub } k;
	size_t idx;
    };
    // A collected renderable region: a LEAF (a vertical flow of lines and edit
    // windows, optionally headed by a tab strip) or a SPLIT (children divided
    // along one axis). `c0`/`width` are the column geometry decided at
    // collection; the rows are the paint half's.
    struct region
    {
	bool is_split;
	ui_split dir;
	long size;
	size_t c0, width;
	std::vector<std::string> header;
	size_t header_active;
	bool header_upper;	// a chrome band's strip: titles uppercase, as the
				// window's panel headers (S3)
	bool floating;		// a floating window's content (S6): a choice
				// lists its options one per row, its label is
				// the window's title
	size_t sel_line;	// the selected option's line (npos = none)
	std::vector<flow_item> order;
	std::vector<line_out> lines;
	std::vector<edit_slot> edits;
	std::vector<region> subs;
	std::vector<region> kids;
	region() : is_split(false), dir(ui_split::none), size(0),
	    c0(0), width(0), header_active(0), header_upper(false),
	    floating(false), sel_line(std::string::npos) {}
    };
    // A FLOATING window (S6): a node hinted `popup` — a pick list with its
    // `dialog` {title, filter?, buttons}, a prompt's `prompt` {label, input},
    // a question's `confirm` {label, choices} — drawn over the workbench as
    // a framed box, its content collected where the walk met it (the
    // focusable slots keep their discovery order).
    struct tui_float
    {
	std::string title;
	bool has_field;			// an input line: the prompt's input or
	std::string field;		// the dialog's filter (the core's text)
	std::vector<std::string> buttons;
	size_t primary;			// the button Enter is (npos = none)
	region content;
	tui_float() : has_field(false), primary(std::string::npos) {}
    };
    std::vector<tui_float> _floats;		// this compose's floating windows
    static void emit_line(region &f, const line_out &l)
    {
	flow_item it;
	it.k = flow_item::kind::line;
	it.idx = f.lines.size();
	f.lines.push_back(l);
	f.order.push_back(it);
    }
    // Divide `total` cells among `sizes` (percent; 0 shares the remainder
    // evenly), `gap` blank cells between adjacent children -- the ONE
    // proportional divider both axes use (vertical columns at collection,
    // horizontal rows at paint). Each child gets at least 1 cell when the total
    // allows; sizes that overflow scale down proportionally.
    static std::vector<size_t> divide_extents(size_t total,
	const std::vector<long> &sizes, size_t gap)
    {
	size_t n = sizes.size();
	std::vector<size_t> ext(n, 0);
	if ( n == 0 )
	    return ext;
	size_t gaps = gap * (n - 1);
	size_t usable = total > gaps ? total - gaps : 0;
	size_t sized_total = 0, sized_n = 0;
	for ( size_t i = 0; i < n; ++i )
	    if ( sizes[i] > 0 )
	    {
		size_t e = usable * (size_t)sizes[i] / 100;
		if ( e < 1 )
		    e = 1;
		ext[i] = e;
		sized_total += e;
		++sized_n;
	    }
	if ( sized_total > usable && sized_total > 0 )
	{
	    for ( size_t i = 0; i < n; ++i )
		if ( sizes[i] > 0 )
		{
		    ext[i] = ext[i] * usable / sized_total;
		    if ( ext[i] < 1 )
			ext[i] = 1;
		}
	    sized_total = 0;
	    for ( size_t i = 0; i < n; ++i )
		if ( sizes[i] > 0 )
		    sized_total += ext[i];
	}
	size_t rest = usable > sized_total ? usable - sized_total : 0;
	size_t unsized_n = n - sized_n;
	if ( unsized_n )
	{
	    size_t each = rest / unsized_n, extra = rest % unsized_n;
	    for ( size_t i = 0; i < n; ++i )
		if ( sizes[i] == 0 )
		{
		    ext[i] = each + (extra ? 1 : 0);
		    if ( extra )
			--extra;
		    if ( ext[i] < 1 )
			ext[i] = 1;
		}
	}
	else if ( rest )
	    ext[n - 1] += rest;
	return ext;
    }
    // A node's tab strip (its `tabs` hint, an ARRAY of {title, active?}) as the
    // header line a leaf pane heads its content with; empty when the node
    // carries none (or the integer-marker form).
    static void read_header(const uinode &n, std::vector<std::string> &header,
	size_t &active)
    {
	active = 0;
	if ( !n.hints.is_object() )
	    return;
	const std::map<std::string, madc::value> &o = n.hints.as_object();
	std::map<std::string, madc::value>::const_iterator it = o.find("tabs");
	if ( it == o.end() || !it->second.is_array() )
	    return;
	const std::vector<madc::value> &rows = it->second.as_array();
	for ( size_t i = 0; i < rows.size(); ++i )
	{
	    if ( !rows[i].is_object() )
		continue;
	    std::string title = hint_str(rows[i], "title");
	    if ( title.empty() )
		continue;
	    if ( hint_of(rows[i], "active", 0) != 0 )
		active = header.size();
	    header.push_back(title);
	}
    }
    void collect_flow(const roles &r, const uinode &n, region &fl)
    {
	if ( !fl.floating && hint_of(n.hints, "popup", 0) )
	{
	    collect_float(r, n);
	    return;
	}
	size_t cols = fl.width;
	ui_split sdir;
	if ( n.role == r.group
	  && ui_split_from_name(hint_str(n.hints, "split"), sdir) )
	{
	    flow_item si;
	    si.k = flow_item::kind::sub;
	    si.idx = fl.subs.size();
	    fl.subs.push_back(collect_region(r, n, fl.c0, fl.width));
	    fl.order.push_back(si);
	    return;
	}
	// A node carrying a `tabs` strip (the editor's open files, S3): the
	// strip is its line — the window docks it above the editor the same.
	{
	    std::vector<std::string> titles;
	    size_t active = 0;
	    read_header(n, titles, active);
	    if ( !titles.empty() )
	    {
		emit_line(fl, tab_strip(titles, active, false));
		for ( size_t i = 0; i < n.children.size(); ++i )
		    collect_flow(r, n.children[i], fl);
		return;
	    }
	}
	if ( n.role == r.heading )
	{
	    // Full-width reverse bar: label left, content right.
	    std::string left = " " + prose::text_of(n.label);
	    std::string right = prose::text_of(n.content);
	    line_out l(left);
	    size_t lw = madc::line_width(left), rw = madc::line_width(right);
	    if ( !right.empty() && lw + rw + 2 <= cols )
		l.text += std::string(cols - lw - rw - 1, ' ') + right;
	    span s; s.col = 0; s.len = cols; s.attr = ui_style::reverse();
	    l.spans.push_back(s);
	    emit_line(fl, l);
	}
	else if ( n.role == r.status )
	{
	    line_out l(" " + node_text(n));
	    span s; s.col = 0; s.len = cols; s.attr = ui_style::reverse();
	    l.spans.push_back(s);
	    emit_line(fl, l);
	}
	else if ( n.role == r.content )
	{
	    std::string text = node_text(n);
	    if ( !text.empty() )
	    {
		std::string wrapped = wrap_text(text, cols);
		size_t start = 0;
		for ( size_t i = 0; i <= wrapped.size(); ++i )
		    if ( i == wrapped.size() || wrapped[i] == '\n' )
		    {
			emit_line(fl, line_out(wrapped.substr(start,
								i - start)));
			start = i + 1;
		    }
	    }
	}
	else if ( n.role == r.item )
	{
	    emit_line(fl, line_out("  " + node_text(n)));
	}
	else if ( n.role == r.action )
	{
	    emit_line(fl, line_out("[" + prose::text_of(n.label) + "]"));
	}
	else if ( n.role == r.separator )
	{
	    emit_line(fl, line_out(std::string()));
	}
	else if ( n.role == r.choice )
	{
	    // The menu bar: the SAME options line mode numbers, navigable
	    // here — the selected option renders reverse (criterion 4).
	    // Two PRESENTATIONS of one focusable (hints, data-driven):
	    //   list:1  — the popup-list shape (IDE-10a palettes): label
	    //             row, then ONE OPTION PER ROW, selected reversed.
	    //             Same navigation/choose semantics as the bar.
	    //   focus:1 — autofocus: arrows/enter land on this choice
	    //             without a tab cycle (a palette is modal while
	    //             up; when its node vanishes, focus resets).
	    size_t slot = _focus_st.count();
	    focusable f;
	    f.k = focusable::kind::choice;
	    f.option_count = n.children.size();
	    for ( size_t i = 0; i < n.children.size(); ++i )
	    {
		f.option_actions.push_back(n.children[i].actions.empty()
					   ? (name_id)0
					   : n.children[i].actions[0]);
		f.option_codes.push_back(hint_of(n.children[i].hints, "code", 0));
	    }
	    _focus_st.add(f);
	    if ( hint_of(n.hints, "focus", 0) )
		_focus_st.set_focus(slot);
	    size_t sel = selection_of(slot);
	    // A floating window's choice is always the list shape; its label
	    // is the window's title (collect_float), not a row.
	    if ( fl.floating || hint_of(n.hints, "list", 0) )
	    {
		if ( !n.label.is_null() && !fl.floating )
		    emit_line(fl, line_out(prose::text_of(n.label)));
		for ( size_t i = 0; i < n.children.size(); ++i )
		{
		    line_out l;
		    l.text = "  " + node_text(n.children[i]);
		    if ( i == sel )
		    {
			span s;
			s.col = 0;
			s.len = l.text.size();
			s.attr = _chrome[(size_t)tui_chrome::list_selected];
			l.spans.push_back(s);
			fl.sel_line = fl.lines.size();
		    }
		    emit_line(fl, l);
		}
		return;	// options consumed — no generic child recursion
	    }
	    line_out l;
	    if ( !n.label.is_null() )
		l.text = prose::text_of(n.label) + " ";
	    for ( size_t i = 0; i < n.children.size(); ++i )
	    {
		std::string opt = " " + node_text(n.children[i]) + " ";
		if ( i == sel )
		{
		    span s;
		    s.col = l.text.size();
		    s.len = opt.size();
		    s.attr = ui_style::reverse();
		    l.spans.push_back(s);
		}
		l.text += opt;
		if ( i + 1 < n.children.size() )
		    l.text += " ";
	    }
	    emit_line(fl, l);
	    return;	// options consumed — no generic child recursion
	}
	else if ( n.role == r.edit )
	{
	    edit_slot e;
	    e.slot = _focus_st.count();
	    e.text = prose::text_of(n.content);
	    e.caret = hint_of(n.hints, "caret", 0);
	    e.sel_start = hint_of(n.hints, "sel_start", -1);
	    e.sel_end = hint_of(n.hints, "sel_end", -1);
	    e.tabw = hint_of(n.hints, "tabwidth", tab_stop);
	    if ( e.tabw < 1 )
		e.tabw = 1;
	    if ( e.tabw > 16 )
		e.tabw = 16;
	    e.rows = hint_of(n.hints, "rows", 0);
	    if ( e.rows < 0 )
		e.rows = 0;
	    e.gutter = hint_of(n.hints, "gutter", 0) != 0;
	    // The same autofocus hint the choice arm honors (IDE-9e: the
	    // active window's edit node carries it).
	    if ( hint_of(n.hints, "focus", 0) )
		_focus_st.set_focus(e.slot);
	    if ( n.hints.is_object() )
	    {
		const std::map<std::string, madc::value> &ho = n.hints.as_object();
		std::map<std::string, madc::value>::const_iterator hi =
		    ho.find("spans");
		if ( hi != ho.end() && hi->second.is_array() )
		    for ( const madc::value &row : hi->second.as_array() )
		    {
			if ( !row.is_object() )
			    continue;
			doc_span ds;
			ds.start = hint_of(row, "s", -1);
			ds.end = hint_of(row, "e", -1);
			const std::map<std::string, madc::value> &ro =
			    row.as_object();
			std::map<std::string, madc::value>::const_iterator ci =
			    ro.find("c");
			if ( ds.start < 0 || ds.end <= ds.start
			  || ci == ro.end() || !ci->second.is_string()
			  || !ui_style_of(ci->second.as_string(), ds.attr) )
			    continue;
			e.spans.push_back(ds);
		    }
	    }
	    flow_item ei;
	    ei.k = flow_item::kind::edit;
	    ei.idx = fl.edits.size();
	    fl.edits.push_back(e);
	    fl.order.push_back(ei);
	    focusable f;
	    f.k = focusable::kind::edit;
	    f.takes_tab = hint_of(n.hints, "tabkey", 0) != 0;	// the field's tab
	    _focus_st.add(f);
	}
	else if ( n.role == r.list && !n.label.is_null() )
	{
	    emit_line(fl, line_out(prose::text_of(n.label) + ":"));
	}
	// group / list / unknown: structure only — children carry it.

	for ( size_t i = 0; i < n.children.size(); ++i )
	    collect_flow(r, n.children[i], fl);
    }
    // A `popup` node as a floating window (S6): its title, its input line,
    // its buttons from the node's own hints, and its content collected at
    // the widest the screen allows (paint_float shrinks the box to it). A
    // prompt's content is its row's one-line form, so only its field shows.
    void collect_float(const roles &r, const uinode &n)
    {
	tui_float f;
	f.content.floating = true;
	f.content.width = _grid.cols > 12 ? _grid.cols - 8 : 4;
	f.title = prose::text_of(n.label);
	bool body = true;
	const std::map<std::string, madc::value> empty;
	const std::map<std::string, madc::value> &ho =
	    n.hints.is_object() ? n.hints.as_object() : empty;
	std::map<std::string, madc::value>::const_iterator hi;
	if ( (hi = ho.find("dialog")) != ho.end() && hi->second.is_object() )
	{
	    const std::string t = hint_str(hi->second, "title");
	    if ( !t.empty() )
		f.title = t;
	    const std::map<std::string, madc::value> &d = hi->second.as_object();
	    std::map<std::string, madc::value>::const_iterator fi = d.find("filter");
	    if ( fi != d.end() && !fi->second.is_null() )
	    {
		f.has_field = true;
		f.field = hint_str(hi->second, "filter");
	    }
	    std::map<std::string, madc::value>::const_iterator bi = d.find("buttons");
	    if ( bi != d.end() && bi->second.is_array() )
		for ( const madc::value &b : bi->second.as_array() )
		{
		    if ( hint_of(b, "choose", 0) && f.primary == std::string::npos )
			f.primary = f.buttons.size();
		    f.buttons.push_back(hint_str(b, "label"));
		}
	}
	else if ( (hi = ho.find("prompt")) != ho.end() && hi->second.is_object() )
	{
	    f.title = hint_str(hi->second, "label");
	    f.has_field = true;
	    f.field = hint_str(hi->second, "input");
	    body = false;
	}
	else if ( (hi = ho.find("confirm")) != ho.end() && hi->second.is_object() )
	{
	    f.title.clear();
	    const std::map<std::string, madc::value> &c = hi->second.as_object();
	    std::map<std::string, madc::value>::const_iterator ci = c.find("choices");
	    if ( ci != c.end() && ci->second.is_array() )
		for ( const madc::value &b : ci->second.as_array() )
		    f.buttons.push_back(hint_str(b, "label"));
	    if ( !f.buttons.empty() )
		f.primary = 0;
	}
	if ( body )
	    collect_flow(r, n, f.content);
	_floats.push_back(f);
    }

    // A framed box over [r0, r1] x [c0, c1], `body` inside; the frame is its
    // own (it never joins the workbench's dividers) — the caller paints it,
    // in `body`, after what goes inside.
    void paint_box(size_t r0, size_t c0, size_t r1, size_t c1, ui_style body,
		   tui_frame &box)
    {
	box.reset(_grid.rows, _grid.cols);
	for ( size_t r = r0 + 1; r < r1; ++r )
	    _grid.put(r, c0 + 1, std::string(c1 - c0 - 1, ' '), body);
	box.hline(r0, c0, c1);
	box.hline(r1, c0, c1);
	box.vline(c0, r0, r1);
	box.vline(c1, r0, r1);
    }
    // The title on the top border, after the frame is painted.
    void paint_box_title(size_t r0, size_t c0, size_t c1, ui_style body,
			 const std::string &title)
    {
	if ( title.empty() || c1 < c0 + 6 )
	    return;
	std::vector<size_t> col;
	std::string shown = madc::line_layout(" " + title + " ", 0, col);
	size_t room = c1 - c0 - 3;
	if ( col.back() > room )
	    shown = madc::line_columns(shown, 0, room);
	ui_style st = body;
	st.flags |= ui_style::BOLD;
	_grid.put(r0, c0 + 2, shown, st);
    }
    // The shadow of a box: two columns to its right, one row below, offset
    // by one (Turbo Vision's).
    void paint_shadow(size_t r0, size_t c0, size_t r1, size_t c1)
    {
	const ui_style shadow = _chrome[(size_t)tui_chrome::shadow];
	for ( size_t r = r0 + 1; r <= r1 && r < _grid.rows; ++r )
	    _grid.overlay_attr(r, c1 + 1, 2, shadow);
	if ( r1 + 1 < _grid.rows )
	    _grid.overlay_attr(r1 + 1, c0 + 2, c1 - c0 + 1, shadow);
    }

    // One floating window, centred, in the upper third: the title on its
    // border, the field (the cursor at its end), the content's rows scrolled
    // to keep the selected one in view (lit across the box), the buttons on
    // the last inner row (the primary in button_primary). The box is as wide
    // as its widest part, at most the screen less a margin.
    void paint_float(const tui_float &f)
    {
	const size_t rows = _grid.rows, cols = _grid.cols;
	if ( rows < 5 || cols < 16 )
	    return;
	const ui_style body = _chrome[(size_t)tui_chrome::dialog];
	std::string bline;
	std::vector<size_t> bcol;	// each button's start in bline
	for ( size_t i = 0; i < f.buttons.size(); ++i )
	{
	    if ( i )
		bline += "  ";
	    bcol.push_back(bline.size());
	    bline += "[ " + f.buttons[i] + " ]";
	}
	size_t iw = 24;
	const std::vector<line_out> &lines = f.content.lines;
	for ( size_t i = 0; i < lines.size(); ++i )
	    iw = std::max(iw, madc::line_width(lines[i].text));
	iw = std::max(iw, madc::line_width(f.title) + 4);
	iw = std::max(iw, madc::line_width(bline));
	if ( f.has_field )
	    iw = std::max(iw, madc::line_width(f.field) + 2);
	if ( iw + 6 > cols )
	    iw = cols - 6;
	const size_t extra = (f.has_field ? 1 : 0) + (f.buttons.empty() ? 0 : 1);
	size_t n = lines.size();
	if ( n + extra + 2 > rows - 1 )
	    n = rows - 1 > extra + 2 ? rows - 1 - extra - 2 : 0;
	const size_t w = iw + 4, h = n + extra + 2;
	const size_t c0 = (cols - w) / 2, c1 = c0 + w - 1;
	const size_t r0 = (rows - h) / 3, r1 = r0 + h - 1;
	tui_frame box;
	paint_box(r0, c0, r1, c1, body, box);
	size_t r = r0 + 1;
	if ( f.has_field )
	{
	    const ui_style fs = _chrome[(size_t)tui_chrome::field];
	    std::vector<size_t> col;
	    std::string shown = madc::line_layout(f.field, 0, col);
	    size_t cw = col.back();
	    if ( cw > iw )
	    {
		shown = madc::line_columns(shown, cw - iw, iw);
		cw = iw;
	    }
	    _grid.put(r, c0 + 2, std::string(iw, ' '), fs);
	    _grid.put(r, c0 + 2, shown, fs);
	    _grid.cursor_row = r;
	    _grid.cursor_col = c0 + 2 + (cw < iw ? cw : iw - 1);
	    _grid.cursor_visible = true;
	    ++r;
	}
	size_t top = 0;
	if ( f.content.sel_line != std::string::npos && n > 0
	  && f.content.sel_line >= n )
	    top = f.content.sel_line - n + 1;
	for ( size_t k = 0; k < n && top + k < lines.size(); ++k, ++r )
	{
	    const size_t li = top + k;
	    line_out l = lines[li];
	    std::vector<size_t> col;
	    std::string shown = madc::line_layout(l.text, 0, col);
	    if ( col.back() > iw )
		l.text = madc::line_columns(shown, 0, iw);
	    for ( size_t s = 0; s < l.spans.size(); ++s )
		if ( l.spans[s].col + l.spans[s].len > l.text.size() )
		    l.spans[s].len = l.spans[s].col < l.text.size()
				     ? l.text.size() - l.spans[s].col : 0;
	    if ( li == f.content.sel_line )
	    {
		const ui_style sel = _chrome[(size_t)tui_chrome::list_selected];
		_grid.put(r, c0 + 1, std::string(w - 2, ' '), sel);
		if ( !f.has_field )
		{
		    _grid.cursor_row = r;
		    _grid.cursor_col = c0 + 1;
		}
		_grid.put(r, c0 + 2, l.text, sel);
		continue;
	    }
	    _grid.put(r, c0 + 2, l.text, body);
	    for ( size_t s = 0; s < l.spans.size(); ++s )
		_grid.overlay_attr(r, c0 + 2 + madc::line_width(l.text.substr(0, l.spans[s].col)),
				   madc::line_width(l.text.substr(l.spans[s].col, l.spans[s].len)),
				   l.spans[s].attr);
	}
	if ( !f.buttons.empty() )
	{
	    const size_t br = r1 - 1;
	    size_t bw = madc::line_width(bline);
	    size_t bc = c1 - 1 - (bw < iw ? bw : iw);
	    _grid.put(br, bc, bline, body);
	    if ( f.primary < f.buttons.size() )
		_grid.overlay_attr(br, bc + madc::line_width(bline.substr(0, bcol[f.primary])),
				   madc::line_width(f.buttons[f.primary]) + 4,
				   _chrome[(size_t)tui_chrome::button_primary]);
	}
	box.paint(_grid, body);
	paint_box_title(r0, c0, c1, body, f.title);
	paint_shadow(r0, c0, r1, c1);
    }

    // Collect a LEAF pane -- its header (its tabs strip) then its children as a
    // flow -- at the given column geometry.
    region collect_leaf(const roles &r, const uinode &n, size_t c0, size_t width)
    {
	region f;
	f.c0 = c0;
	f.width = width;
	f.size = hint_of(n.hints, "size", 0);
	read_header(n, f.header, f.header_active);
	for ( size_t i = 0; i < n.children.size(); ++i )
	    collect_flow(r, n.children[i], f);
	return f;
    }
    // Collect a region: a `split` group (vertical shares the WIDTH here, one
    // blank column between; horizontal shares the rows at paint) or a leaf.
    region collect_region(const roles &r, const uinode &n, size_t c0, size_t width)
    {
	ui_split dir;
	if ( n.role == r.group
	    && ui_split_from_name(hint_str(n.hints, "split"), dir) )
	{
	    region g;
	    g.is_split = true;
	    g.dir = dir;
	    g.c0 = c0;
	    g.width = width;
	    g.size = hint_of(n.hints, "size", 0);
	    if ( dir == ui_split::vertical )
	    {
		std::vector<long> sizes;
		for ( size_t i = 0; i < n.children.size(); ++i )
		    sizes.push_back(hint_of(n.children[i].hints, "size", 0));
		std::vector<size_t> ext = divide_extents(width, sizes, 1);
		size_t c = c0;
		for ( size_t i = 0; i < n.children.size(); ++i )
		{
		    g.kids.push_back(collect_region(r, n.children[i], c, ext[i]));
		    c += ext[i] + 1;
		}
	    }
	    else
		for ( size_t i = 0; i < n.children.size(); ++i )
		    g.kids.push_back(collect_region(r, n.children[i], c0, width));
	    return g;
	}
	return collect_leaf(r, n, c0, width);
    }

    // A composed line through the one layout rule (tabs, code-point
    // widths). Its styled spans are BYTE positions of the text, placed
    // through the same map (B87: a span after a UTF-8 character landed a
    // column right per extra byte). A position past the text's end is a
    // COLUMN: a full-width bar spans len = cols, the row's width.
    void paint_line(size_t row, size_t col0, const line_out &l)
    {
	std::vector<size_t> col;
	_grid.put(row, col0, madc::line_layout(l.text, 0, col));
	const size_t n = l.text.size();
	for ( size_t i = 0; i < l.spans.size(); ++i )
	{
	    size_t s = l.spans[i].col, e = l.spans[i].col + l.spans[i].len;
	    size_t cs = s <= n ? col[s] : std::max(col[n], s);
	    size_t ce = e <= n ? col[e] : std::max(col[n], e);
	    _grid.fill_attr(row, col0 + cs, ce - cs, l.spans[i].attr);
	}
    }
    // A tab strip as one line: each title padded by a blank, the active
    // one in the scheme's tab_active style, the rest in its tab style; a
    // chrome band's titles uppercase (S3). The ONE strip builder — a leaf
    // pane's header and the editor's open files.
    line_out tab_strip(const std::vector<std::string> &titles, size_t active,
		       bool upper) const
    {
	line_out l;
	for ( size_t i = 0; i < titles.size(); ++i )
	{
	    std::string t = titles[i];
	    if ( upper )
		for ( size_t k = 0; k < t.size(); ++k )
		    if ( t[k] >= 'a' && t[k] <= 'z' )
			t[k] = (char)(t[k] - 'a' + 'A');
	    std::string seg = " " + t + " ";
	    const ui_style &st = _chrome[(size_t)(i == active ? tui_chrome::tab_active
							      : tui_chrome::tab)];
	    if ( !st.is_normal() )
	    {
		span s;
		s.col = l.text.size();
		s.len = seg.size();
		s.attr = st;
		l.spans.push_back(s);
	    }
	    l.text += seg;
	}
	return l;
    }
    // A leaf pane's header line: its tab strip.
    void paint_header(size_t row, size_t col0,
	const std::vector<std::string> &titles, size_t active, bool upper)
    {
	paint_line(row, col0, tab_strip(titles, active, upper));
    }

    // A document line's byte->display-column map is madc::line_layout's
    // (madcdis/text_utf16.h): tabs to the next `tabw` stop (8, JOE's default;
    // the ^T option's hint), code-point widths, control bytes as ^X — the ONE
    // map the caret, the horizontal shift, the selection and the highlight
    // spans all convert through, and the one the line editor paints with.
    enum { tab_stop = 8 };

    // THE byte-range-to-visible-row overlap rule (selection and highlight
    // spans both paint through it): the [s0, e0) document range's overlap
    // with the line [begin..end] shown at `row`, converted to display
    // columns through the line's expansion map, honoring the horizontal
    // shift and the column clip.
    // `overlay`: the range keeps the background under it when `attr` names
    // none (a syntax span on the caret line).
    void fill_range_overlap(size_t row, size_t col0, size_t begin, size_t end,
	const std::vector<size_t> &dcol, size_t shift, size_t width,
	long s0, long e0, ui_style attr, bool overlay = false)
    {
	if ( s0 < 0 || e0 <= s0 )
	    return;
	size_t s = (size_t)s0 < begin ? begin : (size_t)s0;
	size_t t = (size_t)e0 > end ? end : (size_t)e0;
	if ( s >= t )
	    return;
	size_t ds = dcol[s - begin];
	size_t dt = dcol[t - begin];
	if ( ds < dt && ds < shift + width && dt > shift )
	{
	    size_t c0 = ds < shift ? 0 : ds - shift;
	    size_t c1 = dt - shift;
	    if ( c1 > width )
		c1 = width;
	    if ( overlay )
		_grid.overlay_attr(row, col0 + c0, c1 - c0, attr);
	    else
		_grid.fill_attr(row, col0 + c0, c1 - c0, attr);
	}
    }

    // Emit one edit region: a window of the document, scrolled to keep
    // the caret visible, selection byte-range highlighted, the grid
    // cursor on the caret when this edit holds focus.
    void paint_edit(const edit_slot &e, size_t top_row, size_t col0,
		    size_t height, size_t width)
    {
	// The line-number gutter (S2): the numbers right-aligned in at least
	// three columns, two blank columns before the text; the text window
	// is what remains. Too narrow a pane keeps its text and no gutter.
	size_t gutter_w = 0;
	size_t total_lines = 1;
	// Line starts (byte offsets); the end sentinel makes every offset
	// belong to exactly one line, the caret-at-EOF position included.
	std::vector<size_t> starts;
	starts.push_back(0);
	for ( size_t i = 0; i < e.text.size(); ++i )
	    if ( e.text[i] == '\n' )
		starts.push_back(i + 1);
	total_lines = starts.size();
	if ( e.gutter )
	{
	    size_t digits = 1;
	    for ( size_t n = total_lines; n >= 10; n /= 10 )
		++digits;
	    gutter_w = (digits < 3 ? 3 : digits) + 3;
	    if ( width <= gutter_w + 1 )
		gutter_w = 0;
	}
	const size_t num_col0 = col0, num_w = gutter_w;
	col0 += gutter_w;
	width -= gutter_w;
	size_t caret = e.caret < 0 ? 0
		     : ((size_t)e.caret > e.text.size() ? e.text.size()
							: (size_t)e.caret);
	size_t caret_line = 0;
	while ( caret_line + 1 < starts.size() && starts[caret_line + 1] <= caret )
	    ++caret_line;

	// The caret's DISPLAY column (tab-aware) — the shift and the grid
	// cursor live in display columns; byte offsets convert through the
	// caret line's expansion map.
	size_t caret_begin = starts[caret_line];
	size_t caret_end = caret_line + 1 < starts.size()
			 ? starts[caret_line + 1] - 1 : e.text.size();
	std::vector<size_t> caret_dcol;
	madc::line_layout(e.text.substr(caret_begin, caret_end - caret_begin),
			  0, caret_dcol, (size_t)e.tabw);
	size_t caret_col = caret_dcol[caret - caret_begin];

	size_t &top = _scroll[e.slot];
	if ( caret_line < top )
	    top = caret_line;
	if ( caret_line >= top + height )
	    top = caret_line - height + 1;
	if ( top >= starts.size() )
	    top = starts.size() ? starts.size() - 1 : 0;
	size_t &shift = _hshift[e.slot];
	shift = caret_col < width ? 0 : caret_col - width + 1;

	for ( size_t k = 0; k < height; ++k )
	{
	    size_t li = top + k;
	    if ( li >= starts.size() )
		break;
	    size_t begin = starts[li];
	    size_t end = li + 1 < starts.size() ? starts[li + 1] - 1
						: e.text.size();
	    std::vector<size_t> dcol;
	    std::string disp = madc::line_layout(e.text.substr(begin, end - begin),
						 0, dcol, (size_t)e.tabw);
	    if ( shift < dcol.back() )
		_grid.put(top_row + k, col0, madc::line_columns(disp, shift, width));
	    if ( num_w )
	    {
		std::string num = std::to_string(li + 1);
		std::string field(num_w - 2 > num.size() ? num_w - 2 - num.size() : 0, ' ');
		bool cur = li == caret_line;
		_grid.put(top_row + k, num_col0, field + num,
			  _chrome[(size_t)(cur ? tui_chrome::gutter_current
					       : tui_chrome::gutter)]);
		// The caret line, faintly (a theme that leaves it normal shows
		// nothing): under the text, so the spans keep its background.
		if ( cur && !_chrome[(size_t)tui_chrome::current_line].is_normal() )
		    _grid.fill_attr(top_row + k, col0, width,
				    _chrome[(size_t)tui_chrome::current_line]);
	    }
	    // Highlight spans first, the selection LAST (it wins where
	    // they overlap) — both are the one range-overlap rule below.
	    for ( size_t si = 0; si < e.spans.size(); ++si )
		fill_range_overlap(top_row + k, col0, begin, end, dcol, shift, width,
				   e.spans[si].start, e.spans[si].end,
				   e.spans[si].attr, true);
	    if ( e.sel_start >= 0 && e.sel_end > e.sel_start )
		fill_range_overlap(top_row + k, col0, begin, end, dcol, shift, width,
				   e.sel_start, e.sel_end,
				   ui_style::reverse());
	    if ( li == caret_line && e.slot == _focus_st.focus() )
	    {
		_grid.cursor_row = top_row + k;
		_grid.cursor_col = col0 + caret_col - shift;
		_grid.cursor_visible = true;
	    }
	}
    }
    // A status node the layout docks at the BOTTOM edge (region statusbar,
    // side bottom — facelift S3).
    static bool is_bottom_status_bar(const roles &r, const uinode &n)
    {
	return n.role == r.status && hint_str(n.hints, "region") == "statusbar"
	    && hint_str(n.hints, "side") == "bottom";
    }
    // The status bar's line: its `items` segments ({seat, label, text}) —
    // the left side from the left edge, the right side against the right,
    // each label dim and the file name (seat `n`) bold, on the scheme's
    // statusbar style; a node without items shows its text.
    line_out status_bar_line(const uinode &n, size_t cols) const
    {
	const ui_style bar = _chrome[(size_t)tui_chrome::statusbar];
	ui_style label = bar;
	label.flags |= ui_style::DIM;
	ui_style name = bar;
	name.flags |= ui_style::BOLD;
	line_out sides[2];
	static const char *const keys[2] = { "left", "right" };
	const madc::value *items = NULL;
	if ( n.hints.is_object() )
	{
	    const std::map<std::string, madc::value> &ho = n.hints.as_object();
	    std::map<std::string, madc::value>::const_iterator ii = ho.find("items");
	    if ( ii != ho.end() && ii->second.is_object() )
		items = &ii->second;
	}
	if ( !items )
	    sides[0] = line_out(" " + node_text(n));
	for ( int k = 0; items && k < 2; ++k )
	{
	    const std::map<std::string, madc::value> &io = items->as_object();
	    std::map<std::string, madc::value>::const_iterator si = io.find(keys[k]);
	    if ( si == io.end() || !si->second.is_array() )
		continue;
	    line_out &l = sides[k];
	    for ( const madc::value &seg : si->second.as_array() )
	    {
		if ( !seg.is_object() )
		    continue;
		std::string lab = hint_str(seg, "label"), txt = hint_str(seg, "text");
		if ( txt.empty() )
		    continue;
		l.text += l.text.empty() ? " " : "  ";
		if ( !lab.empty() )
		{
		    span s; s.col = l.text.size(); s.len = lab.size(); s.attr = label;
		    l.spans.push_back(s);
		    l.text += lab + " ";
		}
		if ( hint_str(seg, "seat") == "n" )
		{
		    span s; s.col = l.text.size(); s.len = txt.size(); s.attr = name;
		    l.spans.push_back(s);
		}
		l.text += txt;
	    }
	}
	if ( !sides[1].text.empty() )
	    sides[1].text += " ";
	// Justify: the right side against the right edge when both fit.
	line_out out = sides[0];
	size_t lw = madc::line_width(out.text), rw = madc::line_width(sides[1].text);
	if ( rw && lw + rw + 1 <= cols )
	{
	    out.text += std::string(cols - lw - rw, ' ');
	    size_t at = out.text.size();
	    for ( const span &sp : sides[1].spans )
	    {
		span s = sp;
		s.col += at;
		out.spans.push_back(s);
	    }
	    out.text += sides[1].text;
	}
	// The bar's own style under the segments' (a span past the text is a
	// column: the whole row).
	span whole; whole.col = 0; whole.len = cols; whole.attr = bar;
	out.spans.insert(out.spans.begin(), whole);
	return out;
    }
    // This compose's chrome styles from the root's `chrome` hint object
    // ({ "<tui_chrome name>": "<style spec>" }): an unknown name or a spec
    // the parser refuses leaves that element's default — the gutter dim, the
    // rest normal.
    void read_chrome(const uinode &tree)
    {
	for ( size_t i = 0; i < (size_t)tui_chrome::count; ++i )
	    _chrome[i] = ui_style::normal();
	_chrome[(size_t)tui_chrome::gutter].flags = ui_style::DIM;
	_chrome[(size_t)tui_chrome::tab_active] = ui_style::reverse();
	_chrome[(size_t)tui_chrome::statusbar] = ui_style::reverse();
	_chrome[(size_t)tui_chrome::menubar] = ui_style::reverse();
	_chrome[(size_t)tui_chrome::menu] = ui_style::reverse();
	_chrome[(size_t)tui_chrome::menu_hot].flags = ui_style::UNDERLINE;
	_chrome[(size_t)tui_chrome::shadow].bg = 1;	// black
	_chrome[(size_t)tui_chrome::shadow].flags = ui_style::DIM;
	_chrome[(size_t)tui_chrome::list_selected] = ui_style::reverse();
	_chrome[(size_t)tui_chrome::field].flags = ui_style::UNDERLINE;
	_chrome[(size_t)tui_chrome::button_primary] = ui_style::reverse();
	if ( !tree.hints.is_object() )
	    return;
	const std::map<std::string, madc::value> &ho = tree.hints.as_object();
	std::map<std::string, madc::value>::const_iterator ci = ho.find("chrome");
	if ( ci == ho.end() || !ci->second.is_object() )
	    return;
	for ( const auto &kv : ci->second.as_object() )
	{
	    tui_chrome k;
	    ui_style st;
	    if ( kv.second.is_string() && tui_chrome_of(kv.first, k)
	      && ui_style_of(kv.second.as_string(), st) )
		_chrome[(size_t)k] = st;
	}
    }
    // Paint a collected region into the row range [r0, r0+h) (columns fixed at
    // collection).
    void paint_region(const region &g, size_t r0, size_t h)
    {
	if ( g.is_split )
	    paint_split(g, r0, h);
	else
	    paint_flow(g, r0, h);
    }
    void paint_split(const region &g, size_t r0, size_t h)
    {
	if ( g.dir == ui_split::horizontal )
	{
	    std::vector<long> sizes;
	    for ( size_t i = 0; i < g.kids.size(); ++i )
		sizes.push_back(g.kids[i].size);
	    std::vector<size_t> ext = divide_extents(h, sizes, 0);
	    size_t row = r0;
	    for ( size_t i = 0; i < g.kids.size(); ++i )
	    {
		paint_region(g.kids[i], row, ext[i]);
		row += ext[i];
	    }
	}
	else
	    for ( size_t i = 0; i < g.kids.size(); ++i )
	    {
		paint_region(g.kids[i], r0, h);
		// The blank column between two panes is a divider (S2), reaching
		// a line just above or below it (a panel's divider: the junction).
		if ( i + 1 < g.kids.size() && h > 0 )
		{
		    size_t col = g.kids[i].c0 + g.kids[i].width;
		    size_t a = r0, b = r0 + h - 1;
		    if ( a > 0 && !_frame.empty_at(a - 1, col) )
			--a;
		    if ( !_frame.empty_at(b + 1, col) )
			++b;
		    _frame.vline(col, a, b);
		}
	    }
    }
    // A leaf flow: its header (if any), then lines/edits/subs packed top to
    // bottom -- a rows:N edit is fixed; among the unhinted, the FIRST (an edit
    // or a nested split) is flexible, the rest one row (byte-identical when
    // nothing is hinted).
    void paint_flow(const region &f, size_t r0, size_t h)
    {
	if ( !f.header.empty() && h > 0 )
	{
	    paint_header(r0, f.c0, f.header, f.header_active, f.header_upper);
	    ++r0;
	    --h;
	}
	size_t fixed = f.lines.size();
	size_t hinted_sum = 0, unhinted = 0;
	for ( size_t i = 0; i < f.edits.size(); ++i )
	{
	    if ( f.edits[i].rows > 0 )
		hinted_sum += (size_t)f.edits[i].rows;
	    else
		++unhinted;
	}
	unhinted += f.subs.size();
	size_t flexible = 0;
	if ( unhinted )
	{
	    size_t others = unhinted - 1;
	    size_t taken = fixed + hinted_sum + others;
	    flexible = h > taken ? h - taken : 1;
	}
	size_t row = r0;
	bool flex_spent = false;
	for ( size_t oi = 0; oi < f.order.size() && row < r0 + h; ++oi )
	{
	    const flow_item &it = f.order[oi];
	    if ( it.k == flow_item::kind::line )
	    {
		paint_line(row, f.c0, f.lines[it.idx]);
		++row;
	    }
	    else if ( it.k == flow_item::kind::edit )
	    {
		const edit_slot &e = f.edits[it.idx];
		size_t he;
		if ( e.rows > 0 )
		    he = (size_t)e.rows;
		else if ( !flex_spent )
		{
		    he = flexible;
		    flex_spent = true;
		}
		else
		    he = 1;
		if ( he > r0 + h - row )
		    he = r0 + h - row;
		paint_edit(e, row, f.c0, he, f.width);
		row += he;
	    }
	    else
	    {
		size_t hs;
		if ( !flex_spent )
		{
		    hs = flexible;
		    flex_spent = true;
		}
		else
		    hs = 1;
		if ( hs > r0 + h - row )
		    hs = r0 + h - row;
		paint_region(f.subs[it.idx], row, hs);
		row += hs;
	    }
	}
    }

public:
    tui_model() : _tb_row(std::string::npos) {}

    const tui_grid &grid() const { return _grid; }
    // Focus and selection are the shared owner's (madcdis/ui_focus.h);
    // the model's spellings forward.
    const std::vector<focusable> &focusables() const { return _focus_st.focusables(); }
    size_t focus_slot() const { return _focus_st.focus(); }
    size_t selection_of(size_t slot) const { return _focus_st.selection_of(slot); }

    // Compose the tree onto a rows x cols grid. The root's children partition
    // into chrome bands (region:sidebar|panel) and the centre flow; a chrome
    // sidebar carves columns (full height), a panel carves rows from the
    // centre; a `split` in the flow and a leaf's tab strip render through the
    // region tree. Edit heights stay DATA (IDE-9e). Byte-identical to the
    // linear stream with no chrome shown and no split (the negative control).
    const tui_grid &compose(const roles &r, const uinode &tree,
	size_t rows, size_t cols)
    {
	_grid.resize(rows, cols);
	_frame.reset(rows, cols);
	read_chrome(tree);
	_focus_st.begin_compose();
	_floats.clear();
	_focus_st.set_menus(read_menus(tree));
	// The menu bar (S4): the root's `menubar` hint (the layout's `menubar`
	// line) keeps it on the top row; without it the bar shows only while
	// open, over the top row (JOE's look keeps its rows).
	const size_t mtop = (hint_of(tree.hints, "menubar", 0) != 0
			     && !_focus_st.menus().empty() && rows > 3) ? 1 : 0;
	// The toolbar (plan §41.11a): a root `toolbar` hint takes the top row
	// (under the bar); the bands and the centre lay out in the rows below
	// it. No hint = no row, byte-identical to before (the negative control).
	const size_t top = mtop + ((rows > mtop + 1 && paint_toolbar(tree, mtop)) ? 1 : 0);
	// The status BAR docked at the bottom (S3, the layout's `status
	// bottom`): the screen's last row, full width, under every band. A
	// status line at the top (JOE's place) stays in the centre flow.
	const uinode *bar = NULL;
	for ( size_t i = 0; i < tree.children.size(); ++i )
	    if ( is_bottom_status_bar(r, tree.children[i]) )
		bar = &tree.children[i];
	if ( bar && rows < top + 3 )
	    bar = NULL;
	const size_t body_rows = rows - top - (bar ? 1 : 0);
	// Column geometry: sidebars carve columns from the full width; the
	// centre keeps the rest; panels span the centre columns.
	size_t centre_c0 = 0, centre_w = cols;
	std::map<size_t, size_t> band_c0, band_w;
	for ( size_t i = 0; i < tree.children.size(); ++i )
	{
	    const uinode &c = tree.children[i];
	    if ( hint_str(c.hints, "region") != "sidebar" )
		continue;
	    ui_side side = ui_side::left;
	    ui_side_from_name(hint_str(c.hints, "side"), side);
	    long sz = hint_of(c.hints, "size", 20);
	    size_t sw = (size_t)((long)cols * sz / 100);
	    if ( sw < 12 )
		sw = 12;
	    if ( centre_w < 2 || sw > centre_w - 1 )
		sw = centre_w > 1 ? centre_w - 1 : 1;
	    size_t bc0;
	    if ( side == ui_side::right )
	    {
		bc0 = centre_c0 + centre_w - sw;
		centre_w -= sw;
	    }
	    else
	    {
		bc0 = centre_c0;
		centre_c0 += sw;
		centre_w -= sw;
	    }
	    // The divider (S2): the band's column next to the centre, a full-
	    // height line; the band's content keeps the rest.
	    if ( sw >= 2 )
	    {
		size_t div = side == ui_side::right ? bc0 : bc0 + sw - 1;
		_frame.vline(div, top, top + body_rows - 1);
		if ( side == ui_side::right )
		    ++bc0;
		--sw;
	    }
	    band_c0[i] = bc0;
	    band_w[i] = sw;
	}
	// Collect in tree order (the focusable slots): the centre flow and each
	// chrome band's content.
	struct band { ui_side side; long size; size_t c0, w; region content; };
	std::vector<band> bands;
	region centre;
	centre.c0 = centre_c0;
	centre.width = centre_w;
	for ( size_t i = 0; i < tree.children.size(); ++i )
	{
	    const uinode &c = tree.children[i];
	    if ( &c == bar )
		continue;			// painted on the last row below
	    std::string reg = hint_str(c.hints, "region");
	    if ( reg == "sidebar" || reg == "panel" )
	    {
		band b;
		ui_side side = ui_side::none;
		ui_side_from_name(hint_str(c.hints, "side"), side);
		b.side = side;
		b.size = hint_of(c.hints, "size", reg == "sidebar" ? 20 : 25);
		if ( reg == "sidebar" )
		{
		    b.c0 = band_c0[i];
		    b.w = band_w[i];
		}
		else
		{
		    b.c0 = centre_c0;
		    b.w = centre_w;
		}
		b.content = collect_leaf(r, c, b.c0, b.w);
		b.content.header_upper = true;
		bands.push_back(b);
	    }
	    else
		collect_flow(r, c, centre);
	}
	_focus_st.end_compose();
	// Row geometry: panels carve rows from the centre; sidebars are full
	// height (below the toolbar row). Paint the panels, the sidebars, then
	// the centre flow.
	size_t centre_r0 = top, centre_h = body_rows;
	for ( size_t i = 0; i < bands.size(); ++i )
	{
	    if ( bands[i].side != ui_side::top && bands[i].side != ui_side::bottom )
		continue;
	    size_t ph = (size_t)((long)body_rows * bands[i].size / 100);
	    if ( ph < 3 )
		ph = 3;
	    if ( centre_h < 2 || ph > centre_h - 1 )
		ph = centre_h > 1 ? centre_h - 1 : 1;
	    size_t pr0;
	    if ( bands[i].side == ui_side::top )
	    {
		pr0 = centre_r0;
		centre_r0 += ph;
	    }
	    else
		pr0 = centre_r0 + centre_h - ph;
	    centre_h -= ph;
	    // The divider (S2): the panel's row next to the centre, across the
	    // centre's columns and onto a sidebar divider beside them (the arms
	    // make the junction).
	    size_t cph = ph, cpr0 = pr0;
	    if ( ph >= 2 )
	    {
		size_t div = bands[i].side == ui_side::top ? pr0 + ph - 1 : pr0;
		size_t h0 = centre_c0, h1 = centre_c0 + centre_w - 1;
		if ( h0 > 0 && !_frame.empty_at(div, h0 - 1) )
		    --h0;
		if ( !_frame.empty_at(div, h1 + 1) )
		    ++h1;
		_frame.hline(div, h0, h1);
		--cph;
		if ( bands[i].side != ui_side::top )
		    ++cpr0;
	    }
	    paint_region(bands[i].content, cpr0, cph);
	}
	for ( size_t i = 0; i < bands.size(); ++i )
	    if ( bands[i].side == ui_side::left || bands[i].side == ui_side::right )
		paint_region(bands[i].content, top, body_rows);
	paint_region(centre, centre_r0, centre_h);
	if ( bar )
	    paint_line(rows - 1, 0, status_bar_line(*bar, cols));
	_frame.paint(_grid, _chrome[(size_t)tui_chrome::divider]);
	// The floating windows over the workbench (S6), then the menus.
	for ( size_t i = 0; i < _floats.size(); ++i )
	    paint_float(_floats[i]);
	// The menu bar and its dropdown LAST: an overlay over everything.
	if ( mtop || _focus_st.menu_is_open() )
	    paint_menus(mtop != 0);
	return _grid;
    }

    // Open the menu titled `title` for the APPLICATION (ui::menu_open — a
    // toolbar button's arrow, facelift S5): it drops under the toolbar
    // button that names it, else under its bar title. False = no menu has
    // that title (the application shows its own list instead).
    bool open_menu(const std::string &title)
    {
	std::map<std::string, size_t>::const_iterator di = _tb_drops.find(title);
	return _focus_st.menu_open_titled(title,
	    di == _tb_drops.end() ? std::string::npos : di->second);
    }

    // The root's `menu` hint ({bar:[{title, items:[{id,code,title,enabled?}
    // |{sep}]}]}, the shape the window's native menu reads) as the focus
    // owner's bar, each title's hotkey letter assigned (menu_hotkeys). A row
    // without an id is dropped, as the window drops it.
    static std::vector<menu_col> read_menus(const uinode &tree)
    {
	std::vector<menu_col> out;
	if ( !tree.hints.is_object() )
	    return out;
	const std::map<std::string, madc::value> &ho = tree.hints.as_object();
	std::map<std::string, madc::value>::const_iterator mi = ho.find("menu");
	if ( mi == ho.end() || !mi->second.is_object() )
	    return out;
	const std::map<std::string, madc::value> &mo = mi->second.as_object();
	std::map<std::string, madc::value>::const_iterator bi = mo.find("bar");
	if ( bi == mo.end() || !bi->second.is_array() )
	    return out;
	for ( const madc::value &mv : bi->second.as_array() )
	{
	    if ( !mv.is_object() )
		continue;
	    menu_col m;
	    m.title = hint_str(mv, "title");
	    const std::map<std::string, madc::value> &mm = mv.as_object();
	    std::map<std::string, madc::value>::const_iterator ii = mm.find("items");
	    if ( ii != mm.end() && ii->second.is_array() )
		for ( const madc::value &iv : ii->second.as_array() )
		{
		    if ( !iv.is_object() )
			continue;
		    menu_row row;
		    if ( hint_of(iv, "sep", 0) )
		    {
			row.sep = true;
			m.rows.push_back(row);
			continue;
		    }
		    row.id = hint_str(iv, "id");
		    if ( row.id.empty() )
			continue;
		    row.title = hint_str(iv, "title");
		    row.code = hint_of(iv, "code", 0);
		    row.enabled = hint_of(iv, "enabled", 1) != 0;
		    m.rows.push_back(row);
		}
	    out.push_back(m);
	}
	menu_hotkeys(out);
	return out;
    }

    // `base` with the hotkey letter's look laid over it: menu_hot's colour
    // when it names one, its attributes added.
    ui_style menu_hot_over(ui_style base) const
    {
	const ui_style &h = _chrome[(size_t)tui_chrome::menu_hot];
	if ( h.fg || h.fg_rgb )
	{
	    base.fg = h.fg;
	    base.fg_rgb = h.fg_rgb;
	}
	base.flags |= h.flags;
	return base;
    }
    // A title at (r, c) in `st`, its hotkey letter (the first occurrence of
    // `hot`, which menu_hotkey_of chose) in menu_hot's look.
    void put_menu_title(size_t r, size_t c, const std::string &title, char hot,
			ui_style st)
    {
	_grid.put(r, c, title, st);
	if ( !hot )
	    return;
	for ( size_t i = 0; i < title.size(); ++i )
	{
	    char ch = title[i];
	    if ( ch >= 'A' && ch <= 'Z' )
		ch = (char)(ch - 'A' + 'a');
	    if ( ch == hot )
	    {
		_grid.overlay_attr(r, c + madc::line_width(title.substr(0, i)), 1,
				   menu_hot_over(st));
		return;
	    }
	}
    }

    // The bar on row 0 (Turbo Vision's: " File  Edit ..." with each title's
    // letter lit) and, while one is open, its dropdown: a framed box under
    // its title, a row per command — the chord the LOADED profile binds to
    // it right-aligned, a disabled row dim, the lit row in menu_selected, a
    // separator a rule across the box — with a shadow to its right and
    // below. Painted after everything, so it covers whatever is there.
    void paint_menus(bool held)
    {
	const std::vector<menu_col> &menus = _focus_st.menus();
	if ( menus.empty() || _grid.rows < 3 || _grid.cols < 8 )
	    return;
	const size_t cols = _grid.cols;
	const size_t open = _focus_st.open_menu();
	// A menu the application dropped from a toolbar button hangs under
	// the button; the bar shows only if the layout holds it.
	const size_t anchor = _focus_st.menu_anchor();
	if ( open < menus.size() && anchor != std::string::npos
	  && _tb_row != std::string::npos )
	{
	    if ( held )
		paint_menu_bar(menus, open);
	    paint_dropdown(menus[open], anchor < cols ? anchor : 0, _tb_row + 1,
			   _focus_st.menu_lit_row());
	    return;
	}
	std::vector<size_t> at = paint_menu_bar(menus, open);
	if ( open >= menus.size() )
	    return;
	paint_dropdown(menus[open], at[open] < cols ? at[open] : 0, 1,
		       _focus_st.menu_lit_row());
    }
    // The bar on row 0, `open`'s title lit; each title's column.
    std::vector<size_t> paint_menu_bar(const std::vector<menu_col> &menus,
				       size_t open)
    {
	const size_t cols = _grid.cols;
	const ui_style bar = _chrome[(size_t)tui_chrome::menubar];
	const ui_style lit = _chrome[(size_t)tui_chrome::menu_selected];
	_grid.put(0, 0, std::string(cols, ' '), bar);
	std::vector<size_t> at(menus.size(), cols);
	size_t c = 1;
	for ( size_t m = 0; m < menus.size() && c < cols; ++m )
	{
	    size_t w = madc::line_width(menus[m].title);
	    at[m] = c;
	    ui_style st = m == open ? lit : bar;
	    _grid.put(0, c, std::string(w + 2, ' '), st);
	    put_menu_title(0, c + 1, menus[m].title, menus[m].hot, st);
	    c += w + 2;
	}
	return at;
    }
    // One menu's dropdown box with its top-left corner at (r0, c0) (moved
    // left to fit the screen; rows past the screen's bottom are cut).
    void paint_dropdown(const menu_col &m, size_t c0, size_t r0, size_t lit_row)
    {
	const ui_style body = _chrome[(size_t)tui_chrome::menu];
	const ui_style lit = _chrome[(size_t)tui_chrome::menu_selected];
	std::vector<std::string> keys(m.rows.size());
	size_t inner = 0;
	for ( size_t i = 0; i < m.rows.size(); ++i )
	{
	    if ( m.rows[i].sep )
		continue;
	    keys[i] = _keys.bindings().chord_for(m.rows[i].code, m.rows[i].id);
	    size_t w = madc::line_width(m.rows[i].title)
		     + (keys[i].empty() ? 0 : 2 + madc::line_width(keys[i]));
	    if ( w > inner )
		inner = w;
	}
	const size_t rows = _grid.rows, cols = _grid.cols;
	size_t w = inner + 4;			// the border and a space each side
	if ( w > cols )
	    w = cols;
	if ( c0 + w > cols )
	    c0 = cols - w;
	size_t h = m.rows.size() + 2;
	if ( r0 + h > rows )
	    h = rows - r0;
	if ( h < 2 || w < 4 )
	    return;
	const size_t r1 = r0 + h - 1, c1 = c0 + w - 1;
	tui_frame box;
	paint_box(r0, c0, r1, c1, body, box);
	for ( size_t r = r0 + 1; r < r1; ++r )
	{
	    const size_t i = r - r0 - 1;
	    const menu_row &row = m.rows[i];
	    if ( row.sep )
	    {
		box.hline(r, c0, c1);
		continue;
	    }
	    ui_style st = i == lit_row ? lit : body;
	    if ( !row.enabled )
		st.flags |= ui_style::DIM;
	    if ( i == lit_row )
		_grid.put(r, c0 + 1, std::string(w - 2, ' '), st);
	    put_menu_title(r, c0 + 2, row.title, row.enabled ? row.hot : 0, st);
	    if ( !keys[i].empty() )
	    {
		size_t kw = madc::line_width(keys[i]);
		if ( kw + 3 <= w )
		    _grid.put(r, c1 - 1 - kw, keys[i], st);
	    }
	    if ( i == lit_row )
	    {
		_grid.cursor_row = r;
		_grid.cursor_col = c0 + 1;
	    }
	}
	box.paint(_grid, body);
	paint_shadow(r0, c0, r1, c1);
    }

    // The picture a toolbar button's icon draws as (S5): the window's shapes
    // in a cell — ▶ run, ▷ debug, ■ stop, ↷ ↓ ↑ the steps, ● breakpoints; the
    // file commands are words (as the plan's target screen). "" = none. A
    // terminal without Unicode spells them in ASCII (ui_glyph_ascii).
    static const char *tui_icon_glyph(ui_icon ic)
    {
	switch ( ic )
	{
	    case ui_icon::run:	       return "\xe2\x96\xb6";	// ▶
	    case ui_icon::debug:       return "\xe2\x96\xb7";	// ▷
	    case ui_icon::stop:	       return "\xe2\x96\xa0";	// ■
	    case ui_icon::step_over:   return "\xe2\x86\xb7";	// ↷
	    case ui_icon::step_into:   return "\xe2\x86\x93";	// ↓
	    case ui_icon::step_out:    return "\xe2\x86\x91";	// ↑
	    case ui_icon::breakpoints: return "\xe2\x97\x8f";	// ●
	    default:		       return "";
	}
    }
    // The toolbar row (plan §41.11a, facelift S5) from the root's `toolbar`
    // hint, at `row`: each button its icon's glyph and its label, two
    // columns apart, a disabled one dim; a button whose `drop` names a menu
    // ends in ▾ (its column recorded: the menu drops under it, open_menu); a
    // separator row a divider. A row with no label or no action is dropped.
    // False (nothing painted) when the root carries none.
    bool paint_toolbar(const uinode &tree, size_t row)
    {
	_tb_row = std::string::npos;
	_tb_drops.clear();
	if ( !tree.hints.is_object() )
	    return false;
	const std::map<std::string, madc::value> &ho = tree.hints.as_object();
	std::map<std::string, madc::value>::const_iterator ti = ho.find("toolbar");
	if ( ti == ho.end() || !ti->second.is_array() )
	    return false;
	const ui_style base = _chrome[(size_t)tui_chrome::toolbar];
	bool any = false, pending_sep = false;
	size_t c = 1;
	for ( const madc::value &b : ti->second.as_array() )
	{
	    if ( !b.is_object() )
		continue;
	    if ( hint_of(b, "sep", 0) )
	    {
		pending_sep = any;		// a divider BETWEEN buttons only
		continue;
	    }
	    const std::string label = hint_str(b, "label");
	    const std::string action = hint_str(b, "action");
	    if ( label.empty() || action.empty() )
		continue;
	    if ( !any )
		_grid.put(row, 0, std::string(_grid.cols, ' '), base);
	    if ( pending_sep )
	    {
		_frame.vline(c - 1, row, row);
		c += 1;
		pending_sep = false;
	    }
	    any = true;
	    ui_style st = base;
	    if ( hint_of(b, "enabled", 1) == 0 )
		st.flags |= ui_style::DIM;
	    const size_t start = c;
	    const char *glyph = tui_icon_glyph((ui_icon)hint_of(b, "icon", 0));
	    if ( *glyph )
	    {
		_grid.put(row, c, glyph, st);
		c += 2;
	    }
	    _grid.put(row, c, label, st);
	    c += madc::line_width(label);
	    if ( b.is_object() && b.as_object().count("drop") )
	    {
		const std::string menu = hint_str(b.as_object().at("drop"), "arg");
		if ( !menu.empty() )
		{
		    _grid.put(row, c + 1, "\xe2\x96\xbe", st);	// ▾
		    c += 2;
		    _tb_drops[menu] = start;
		}
	    }
	    c += 2;
	}
	if ( any )
	    _tb_row = row;
	return any;
    }

    // Install a finalized bindings table (a profile swap is a new table);
    // any chord in flight is abandoned with its profile. The table and the
    // chord in flight live in the key owner (madcdis/keys.h).
    void set_bindings(const tui_bindings &b) { _keys.set_bindings(b); }
    const std::string &pending_chord() const { return _keys.pending(); }

    // Keys -> semantic events against the last compose's focusables:
    // bound sequences resolve FIRST (a pending chord consumes every key
    // until it completes, misses, or esc cancels it — resize alone passes
    // through); then printable runs coalesce into ONE text event (§7.5);
    // tab cycles focus; arrows navigate a focused choice (selection is
    // presentation state — a focus event says "repaint"); enter on a
    // focused choice chooses; everything else reaches the application as
    // a key event. With no table installed, behavior is byte-identical
    // to the pre-bindings adapter. The loop itself is the shared adapter
    // ui_apply_keys (madcdis/ui_input.h) — the DOM model runs the same one.
    std::vector<tui_event> apply_keys(const std::vector<tui_keyev> &keys)
    {
	// The one adapter (madcdis/ui_input.h) over this model's two owners.
	return ui_apply_keys(_keys, _focus_st, keys);
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_MODEL_H
