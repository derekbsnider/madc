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
#include "madcdis/tui_keyparse.h"	// raw bytes -> keys (S0 split)

namespace madc {
namespace hub {

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
    focus_state _focus_st;			// focus slot + per-choice selection + navigation
    std::map<size_t, size_t> _scroll;		// per edit slot: top line
    std::map<size_t, size_t> _hshift;		// per edit slot: left shift
    key_resolver _keys;			// the ONE chord/key owner (madcdis/keys.h)

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
	edit_slot() : line_index(0), slot(0), caret(0),
		      sel_start(-1), sel_end(-1), tabw(tab_stop),
		      rows(0) {}
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
	std::vector<flow_item> order;
	std::vector<line_out> lines;
	std::vector<edit_slot> edits;
	std::vector<region> subs;
	std::vector<region> kids;
	region() : is_split(false), dir(ui_split::none), size(0),
	    c0(0), width(0), header_active(0) {}
    };
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
	    if ( hint_of(n.hints, "list", 0) )
	    {
		if ( !n.label.is_null() )
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
			s.attr = ui_style::reverse();
			l.spans.push_back(s);
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
    // A leaf pane's header line: the tab titles, the active one reverse.
    void paint_header(size_t row, size_t col0,
	const std::vector<std::string> &titles, size_t active)
    {
	line_out l;
	for ( size_t i = 0; i < titles.size(); ++i )
	{
	    std::string seg = " " + titles[i] + " ";
	    if ( i == active )
	    {
		span s;
		s.col = l.text.size();
		s.len = seg.size();
		s.attr = ui_style::reverse();
		l.spans.push_back(s);
	    }
	    l.text += seg;
	}
	paint_line(row, col0, l);
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
    void fill_range_overlap(size_t row, size_t col0, size_t begin, size_t end,
	const std::vector<size_t> &dcol, size_t shift, size_t width,
	long s0, long e0, ui_style attr)
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
	    _grid.fill_attr(row, col0 + c0, c1 - c0, attr);
	}
    }

    // Emit one edit region: a window of the document, scrolled to keep
    // the caret visible, selection byte-range highlighted, the grid
    // cursor on the caret when this edit holds focus.
    void paint_edit(const edit_slot &e, size_t top_row, size_t col0,
		    size_t height, size_t width)
    {
	// Line starts (byte offsets); the end sentinel makes every offset
	// belong to exactly one line, the caret-at-EOF position included.
	std::vector<size_t> starts;
	starts.push_back(0);
	for ( size_t i = 0; i < e.text.size(); ++i )
	    if ( e.text[i] == '\n' )
		starts.push_back(i + 1);
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
	    // Highlight spans first, the selection LAST (it wins where
	    // they overlap) — both are the one range-overlap rule below.
	    for ( size_t si = 0; si < e.spans.size(); ++si )
		fill_range_overlap(top_row + k, col0, begin, end, dcol, shift, width,
				   e.spans[si].start, e.spans[si].end,
				   e.spans[si].attr);
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
		paint_region(g.kids[i], r0, h);
    }
    // A leaf flow: its header (if any), then lines/edits/subs packed top to
    // bottom -- a rows:N edit is fixed; among the unhinted, the FIRST (an edit
    // or a nested split) is flexible, the rest one row (byte-identical when
    // nothing is hinted).
    void paint_flow(const region &f, size_t r0, size_t h)
    {
	if ( !f.header.empty() && h > 0 )
	{
	    paint_header(r0, f.c0, f.header, f.header_active);
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
    tui_model() {}

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
	_focus_st.begin_compose();
	// The toolbar (plan §41.11a): a root `toolbar` hint takes the top row;
	// the bands and the centre lay out in the rows below it. No hint = no
	// row, byte-identical to before (the negative control).
	const std::string tbline = toolbar_line(tree);
	const size_t top = (!tbline.empty() && rows > 1) ? 1 : 0;
	if ( top )
	    _grid.put(0, 0, tbline);
	const size_t body_rows = rows - top;
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
	    paint_region(bands[i].content, pr0, ph);
	}
	for ( size_t i = 0; i < bands.size(); ++i )
	    if ( bands[i].side == ui_side::left || bands[i].side == ui_side::right )
		paint_region(bands[i].content, top, body_rows);
	paint_region(centre, centre_r0, centre_h);
	return _grid;
    }

    // The toolbar's one line (plan §41.11a): each row of the root's
    // `toolbar` hint as `[Label chord]`, the chord the LOADED profile binds
    // to the row's code, else to its name (the page's tooltip, the menu's
    // accelerator: tui_bindings::chord_for). A row with no label or no
    // action is dropped. "" when the root carries none.
    std::string toolbar_line(const uinode &tree) const
    {
	std::string line;
	if ( !tree.hints.is_object() )
	    return line;
	const std::map<std::string, madc::value> &ho = tree.hints.as_object();
	std::map<std::string, madc::value>::const_iterator ti = ho.find("toolbar");
	if ( ti == ho.end() || !ti->second.is_array() )
	    return line;
	const std::vector<madc::value> &tbrows = ti->second.as_array();
	for ( size_t k = 0; k < tbrows.size(); ++k )
	{
	    if ( !tbrows[k].is_object() )
		continue;
	    const std::string label = hint_str(tbrows[k], "label");
	    const std::string action = hint_str(tbrows[k], "action");
	    if ( label.empty() || action.empty() )
		continue;
	    const std::string key = _keys.bindings().chord_for(hint_of(tbrows[k], "code", 0), action);
	    if ( !line.empty() )
		line += ' ';
	    line += "[" + label + (key.empty() ? std::string() : " " + key) + "]";
	}
	return line;
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
