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
//   - focus and selection: choice options are NAVIGABLE here (the same
//     tree line mode numbers — design success criterion 4); focus cycles
//     across choice/edit nodes;
//   - the input adapter: raw terminal bytes → keys (tui_keyparse: CSI/SS3
//     escape parsing with an explicit flush for the bare-ESC pause) and
//     keys → SEMANTIC events, coalescing printable runs into one text
//     event (design §7.5 — five key events never become five domain
//     transactions);
//   - differential support: dirty-row comparison between two grids.
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

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "madcdis/uinode.h"
#include "madcdis/render_text.h"	// wrap_text — the one wrap owner

namespace madc {
namespace hub {

// ------------------------------------------------------------------ the grid
enum class tui_attr : unsigned char
{
    normal = 0,
    reverse = 1
};

struct tui_cell
{
    char     ch;
    tui_attr attr;
    tui_cell() : ch(' '), attr(tui_attr::normal) {}
    bool operator==(const tui_cell &o) const
	{ return ch == o.ch && attr == o.attr; }
    bool operator!=(const tui_cell &o) const { return !(*this == o); }
};

struct tui_grid
{
    size_t rows, cols;
    std::vector<tui_cell> cells;
    size_t cursor_row, cursor_col;	// the physical cursor (an edit caret)
    bool   cursor_visible;

    tui_grid() : rows(0), cols(0), cursor_row(0), cursor_col(0),
		 cursor_visible(false) {}

    void resize(size_t r, size_t c)
    {
	rows = r;
	cols = c;
	cells.assign(r * c, tui_cell());
	cursor_row = cursor_col = 0;
	cursor_visible = false;
    }
    tui_cell &at(size_t r, size_t c) { return cells[r * cols + c]; }
    const tui_cell &at(size_t r, size_t c) const { return cells[r * cols + c]; }

    // Clipped text write; never wraps.
    void put(size_t r, size_t c, const std::string &text,
	     tui_attr attr = tui_attr::normal)
    {
	if ( r >= rows )
	    return;
	for ( size_t i = 0; i < text.size() && c + i < cols; ++i )
	{
	    tui_cell &cell = at(r, c + i);
	    cell.ch = text[i];
	    cell.attr = attr;
	}
    }
    void fill_attr(size_t r, size_t c, size_t len, tui_attr attr)
    {
	if ( r >= rows )
	    return;
	for ( size_t i = 0; i < len && c + i < cols; ++i )
	    at(r, c + i).attr = attr;
    }
    // The row as text, right-trimmed — the unit batteries' view.
    std::string row_text(size_t r) const
    {
	std::string out;
	if ( r >= rows )
	    return out;
	for ( size_t c = 0; c < cols; ++c )
	    out += at(r, c).ch;
	size_t end = out.find_last_not_of(' ');
	return end == std::string::npos ? std::string() : out.substr(0, end + 1);
    }
};

// Rows differing between two grids — what a target repaints. A dimension
// change dirties everything.
inline std::vector<size_t> tui_dirty_rows(const tui_grid &prev,
					  const tui_grid &next)
{
    std::vector<size_t> out;
    if ( prev.rows != next.rows || prev.cols != next.cols )
    {
	for ( size_t r = 0; r < next.rows; ++r )
	    out.push_back(r);
	return out;
    }
    for ( size_t r = 0; r < next.rows; ++r )
	for ( size_t c = 0; c < next.cols; ++c )
	    if ( prev.at(r, c) != next.at(r, c) )
	    {
		out.push_back(r);
		break;
	    }
    return out;
}

// ------------------------------------------------------------------- the keys
enum class tui_key : unsigned char
{
    none = 0,
    ch,		// printable byte in `ch`
    ctrl,	// control chord; `ch` = the lowercase letter (^S -> 's')
    enter, tab, backspace, esc,
    up, down, left, right,
    home, end, pgup, pgdn, del, ins,
    resize	// synthesized by the target on a size change
};

struct tui_keyev
{
    tui_key kind;
    char    ch;
    tui_keyev() : kind(tui_key::none), ch(0) {}
    explicit tui_keyev(tui_key k, char c = 0) : kind(k), ch(c) {}
};

// Raw terminal bytes -> keys: the escape-sequence state machine (CSI and
// SS3 forms of the VT100/xterm family; the shapes every terminal library
// parses — cross-checked against termbox2's and ncurses's tables). A bare
// ESC is ambiguous until the input pauses: the TARGET calls flush() when
// its read times out after an ESC, resolving it to the esc key. Modifier
// parameters on arrows ("1;2A") resolve to the unmodified key in this
// pilot.
class tui_keyparse
{
    enum class state : unsigned char { normal, esc, csi, ss3 };
    state _st;
    std::string _params;

    static void emit(std::vector<tui_keyev> &out, tui_key k, char c = 0)
    {
	out.push_back(tui_keyev(k, c));
    }
    static void resolve_csi(const std::string &params, char final_byte,
			    std::vector<tui_keyev> &out)
    {
	switch ( final_byte )
	{
	    case 'A': emit(out, tui_key::up); return;
	    case 'B': emit(out, tui_key::down); return;
	    case 'C': emit(out, tui_key::right); return;
	    case 'D': emit(out, tui_key::left); return;
	    case 'H': emit(out, tui_key::home); return;
	    case 'F': emit(out, tui_key::end); return;
	    case '~':
		switch ( params.empty() ? 0 : atoi(params.c_str()) )
		{
		    case 1: case 7: emit(out, tui_key::home); return;
		    case 4: case 8: emit(out, tui_key::end); return;
		    case 2: emit(out, tui_key::ins); return;
		    case 3: emit(out, tui_key::del); return;
		    case 5: emit(out, tui_key::pgup); return;
		    case 6: emit(out, tui_key::pgdn); return;
		    default: return;	// unrecognized: dropped
		}
	    default: return;		// unrecognized final: dropped
	}
    }
    void feed_byte(unsigned char b, std::vector<tui_keyev> &out)
    {
	switch ( _st )
	{
	    case state::esc:
		if ( b == '[' )
		{
		    _st = state::csi;
		    _params.clear();
		    return;
		}
		if ( b == 'O' )
		{
		    _st = state::ss3;
		    return;
		}
		// ESC followed by an ordinary byte: the ESC stands alone
		// (alt-chords are a deferred refinement) and the byte is
		// reprocessed normally.
		emit(out, tui_key::esc);
		_st = state::normal;
		feed_byte(b, out);
		return;
	    case state::csi:
		if ( b >= 0x40 && b <= 0x7e )
		{
		    resolve_csi(_params, (char)b, out);
		    _st = state::normal;
		}
		else if ( _params.size() < 16 )
		    _params += (char)b;
		else
		    _st = state::normal;	// runaway sequence: dropped
		return;
	    case state::ss3:
		switch ( b )
		{
		    case 'A': emit(out, tui_key::up); break;
		    case 'B': emit(out, tui_key::down); break;
		    case 'C': emit(out, tui_key::right); break;
		    case 'D': emit(out, tui_key::left); break;
		    case 'H': emit(out, tui_key::home); break;
		    case 'F': emit(out, tui_key::end); break;
		    default: break;		// unrecognized: dropped
		}
		_st = state::normal;
		return;
	    case state::normal:
	    default:
		break;
	}
	if ( b == 0x1b )
	    _st = state::esc;
	else if ( b == '\r' || b == '\n' )
	    emit(out, tui_key::enter);
	else if ( b == '\t' )
	    emit(out, tui_key::tab);
	else if ( b == 0x7f || b == 0x08 )
	    emit(out, tui_key::backspace);
	else if ( b >= 0x01 && b <= 0x1a )
	    emit(out, tui_key::ctrl, (char)('a' + b - 1));
	else if ( b >= 0x20 && b <= 0x7e )
	    emit(out, tui_key::ch, (char)b);
	// 0x00, 0x1c..0x1f, >=0x80: dropped (byte-oriented pilot; UTF-8
	// glyph handling is the named residue).
    }

public:
    tui_keyparse() : _st(state::normal) {}

    void feed(const char *bytes, size_t n, std::vector<tui_keyev> &out)
    {
	for ( size_t i = 0; i < n; ++i )
	    feed_byte((unsigned char)bytes[i], out);
    }
    // Mid-sequence? The target polls briefly only then — an unambiguous
    // batch pays zero added latency.
    bool pending() const { return _st != state::normal; }
    // The input paused: a pending bare ESC is the esc key; a partial
    // CSI/SS3 is line noise and drops.
    void flush(std::vector<tui_keyev> &out)
    {
	if ( _st == state::esc )
	    emit(out, tui_key::esc);
	_st = state::normal;
	_params.clear();
    }
};

// ----------------------------------------------------------------- the events
// What the application receives: SEMANTIC units, never raw terminal
// events. The target/model pair owns which keys become navigation
// (consumed here, re-render signalled) and which reach the application.
enum class tui_event_kind : unsigned char
{
    none = 0,
    text,	// a coalesced printable run — one semantic insertion
    key,	// a non-printable key for the application to interpret
    choose,	// enter on the focused choice's selected option
    focus,	// focus or menu selection moved: recompose and repaint
    resize	// the surface changed size: recompose and repaint
};

struct tui_event
{
    tui_event_kind kind;
    std::string	   text;	// text: the run
    tui_key	   key;		// key: which one (ctrl -> `ch`)
    char	   ch;
    size_t	   option;	// choose: 0-based option index
    name_id	   action;	// choose: the option's first action; 0 = none

    tui_event() : kind(tui_event_kind::none), key(tui_key::none), ch(0),
		  option(0), action(0) {}
};

// ------------------------------------------------------------------ the model
// One instance per TUI session. Contract: compose() before apply_keys()
// (events are interpreted against the focusables the last compose
// discovered), recompose after any focus/resize/text event. Focusable
// identity is discovery order — stable while the application composes
// the same tree shape, which is the Phase-1 contract.
class tui_model
{
public:
    struct focusable
    {
	enum class kind : unsigned char { choice, edit };
	kind k;
	size_t option_count;			// choice: how many options
	std::vector<name_id> option_actions;	// choice: first action each
	focusable() : k(kind::choice), option_count(0) {}
    };

private:
    tui_grid _grid;
    std::vector<focusable> _focusables;
    size_t _focus;
    std::map<size_t, size_t> _selection;	// per choice slot
    std::map<size_t, size_t> _scroll;		// per edit slot: top line
    std::map<size_t, size_t> _hshift;		// per edit slot: left shift

    // One composed output line: text plus attribute spans.
    struct span { size_t col, len; tui_attr attr; };
    struct line_out
    {
	std::string text;
	std::vector<span> spans;
	line_out() {}
	explicit line_out(const std::string &t) : text(t) {}
    };
    // A flexible edit region parked between fixed lines.
    struct edit_slot
    {
	size_t line_index;	// position in the fixed-line stream
	size_t slot;		// focusable index
	std::string text;	// the bound document text
	long caret;		// byte offsets from the node's hints
	long sel_start, sel_end;
	edit_slot() : line_index(0), slot(0), caret(0),
		      sel_start(-1), sel_end(-1) {}
    };

    static long hint_of(const madc::value &hints, const char *key, long dflt)
    {
	if ( !hints.is_object() )
	    return dflt;
	const std::map<std::string, madc::value> &o = hints.as_object();
	std::map<std::string, madc::value>::const_iterator it = o.find(key);
	if ( it == o.end() || !it->second.is_integer() )
	    return dflt;
	return (long)it->second.as_integer();
    }

    void walk(const roles &r, const uinode &n, size_t cols,
	      std::vector<line_out> &lines, std::vector<edit_slot> &edits)
    {
	if ( n.role == r.heading )
	{
	    // Full-width reverse bar: label left, content right.
	    std::string left = " " + prose::text_of(n.label);
	    std::string right = prose::text_of(n.content);
	    line_out l(left);
	    if ( !right.empty() && left.size() + right.size() + 2 <= cols )
		l.text += std::string(cols - left.size() - right.size() - 1,
				      ' ') + right;
	    span s; s.col = 0; s.len = cols; s.attr = tui_attr::reverse;
	    l.spans.push_back(s);
	    lines.push_back(l);
	}
	else if ( n.role == r.status )
	{
	    line_out l(" " + node_text(n));
	    span s; s.col = 0; s.len = cols; s.attr = tui_attr::reverse;
	    l.spans.push_back(s);
	    lines.push_back(l);
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
			lines.push_back(line_out(wrapped.substr(start,
								i - start)));
			start = i + 1;
		    }
	    }
	}
	else if ( n.role == r.item )
	{
	    lines.push_back(line_out("  " + node_text(n)));
	}
	else if ( n.role == r.action )
	{
	    lines.push_back(line_out("[" + prose::text_of(n.label) + "]"));
	}
	else if ( n.role == r.separator )
	{
	    lines.push_back(line_out(std::string()));
	}
	else if ( n.role == r.choice )
	{
	    // The menu bar: the SAME options line mode numbers, navigable
	    // here — the selected option renders reverse (criterion 4).
	    size_t slot = _focusables.size();
	    focusable f;
	    f.k = focusable::kind::choice;
	    f.option_count = n.children.size();
	    for ( size_t i = 0; i < n.children.size(); ++i )
		f.option_actions.push_back(n.children[i].actions.empty()
					   ? (name_id)0
					   : n.children[i].actions[0]);
	    _focusables.push_back(f);
	    size_t sel = selection_of(slot);
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
		    s.attr = tui_attr::reverse;
		    l.spans.push_back(s);
		}
		l.text += opt;
		if ( i + 1 < n.children.size() )
		    l.text += " ";
	    }
	    lines.push_back(l);
	    return;	// options consumed — no generic child recursion
	}
	else if ( n.role == r.edit )
	{
	    edit_slot e;
	    e.line_index = lines.size();
	    e.slot = _focusables.size();
	    e.text = prose::text_of(n.content);
	    e.caret = hint_of(n.hints, "caret", 0);
	    e.sel_start = hint_of(n.hints, "sel_start", -1);
	    e.sel_end = hint_of(n.hints, "sel_end", -1);
	    edits.push_back(e);
	    focusable f;
	    f.k = focusable::kind::edit;
	    _focusables.push_back(f);
	}
	else if ( n.role == r.list && !n.label.is_null() )
	{
	    lines.push_back(line_out(prose::text_of(n.label) + ":"));
	}
	// group / list / unknown: structure only — children carry it.

	for ( size_t i = 0; i < n.children.size(); ++i )
	    walk(r, n.children[i], cols, lines, edits);
    }

    void paint_line(size_t row, const line_out &l)
    {
	_grid.put(row, 0, l.text);
	for ( size_t i = 0; i < l.spans.size(); ++i )
	    _grid.fill_attr(row, l.spans[i].col, l.spans[i].len,
			    l.spans[i].attr);
    }

    // Emit one edit region: a window of the document, scrolled to keep
    // the caret visible, selection byte-range highlighted, the grid
    // cursor on the caret when this edit holds focus.
    void paint_edit(const edit_slot &e, size_t top_row, size_t height,
		    size_t cols)
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
	size_t caret_col = caret - starts[caret_line];

	size_t &top = _scroll[e.slot];
	if ( caret_line < top )
	    top = caret_line;
	if ( caret_line >= top + height )
	    top = caret_line - height + 1;
	if ( top >= starts.size() )
	    top = starts.size() ? starts.size() - 1 : 0;
	size_t &shift = _hshift[e.slot];
	shift = caret_col < cols ? 0 : caret_col - cols + 1;

	for ( size_t k = 0; k < height; ++k )
	{
	    size_t li = top + k;
	    if ( li >= starts.size() )
		break;
	    size_t begin = starts[li];
	    size_t end = li + 1 < starts.size() ? starts[li + 1] - 1
						: e.text.size();
	    std::string line = e.text.substr(begin, end - begin);
	    if ( shift < line.size() )
		_grid.put(top_row + k, 0, line.substr(shift, cols));
	    // Selection highlight: this line's overlap with the range.
	    if ( e.sel_start >= 0 && e.sel_end > e.sel_start )
	    {
		size_t s = (size_t)e.sel_start < begin ? begin
						       : (size_t)e.sel_start;
		size_t t = (size_t)e.sel_end > end ? end : (size_t)e.sel_end;
		if ( s < t && s - begin < shift + cols && t - begin > shift )
		{
		    size_t c0 = s - begin < shift ? 0 : s - begin - shift;
		    size_t c1 = t - begin - shift;
		    _grid.fill_attr(top_row + k, c0, c1 - c0,
				    tui_attr::reverse);
		}
	    }
	    if ( li == caret_line && e.slot == _focus )
	    {
		_grid.cursor_row = top_row + k;
		_grid.cursor_col = caret_col - shift;
		_grid.cursor_visible = true;
	    }
	}
    }

public:
    tui_model() : _focus(0) {}

    const tui_grid &grid() const { return _grid; }
    const std::vector<focusable> &focusables() const { return _focusables; }
    size_t focus_slot() const { return _focus; }
    size_t selection_of(size_t slot) const
    {
	std::map<size_t, size_t>::const_iterator it = _selection.find(slot);
	size_t sel = it == _selection.end() ? 0 : it->second;
	if ( slot < _focusables.size()
	  && _focusables[slot].k == focusable::kind::choice
	  && _focusables[slot].option_count > 0
	  && sel >= _focusables[slot].option_count )
	    sel = _focusables[slot].option_count - 1;
	return sel;
    }

    // Compose the tree onto a rows×cols grid. The FIRST edit node is the
    // flexible region (it absorbs the rows fixed content leaves free);
    // later edit nodes get one row each. The tree arrives already
    // access-filtered — composition makes no gate decision.
    const tui_grid &compose(const roles &r, const uinode &tree,
			    size_t rows, size_t cols)
    {
	_grid.resize(rows, cols);
	_focusables.clear();
	std::vector<line_out> lines;
	std::vector<edit_slot> edits;
	walk(r, tree, cols, lines, edits);
	if ( _focus >= _focusables.size() )
	    _focus = 0;

	size_t fixed = lines.size();
	size_t flexible = 0;
	if ( !edits.empty() )
	{
	    size_t others = edits.size() - 1;	// one row each
	    flexible = rows > fixed + others ? rows - fixed - others : 1;
	}
	size_t row = 0, li = 0, ei = 0;
	while ( row < rows && (li < lines.size() || ei < edits.size()) )
	{
	    if ( ei < edits.size() && edits[ei].line_index == li )
	    {
		size_t h = ei == 0 ? flexible : 1;
		if ( h > rows - row )
		    h = rows - row;
		paint_edit(edits[ei], row, h, cols);
		row += h;
		++ei;
		continue;
	    }
	    if ( li < lines.size() )
	    {
		paint_line(row, lines[li]);
		++row;
		++li;
	    }
	}
	return _grid;
    }

    // Keys -> semantic events against the last compose's focusables:
    // printable runs coalesce into ONE text event (§7.5); tab cycles
    // focus; arrows navigate a focused choice (selection is presentation
    // state — a focus event says "repaint"); enter on a focused choice
    // chooses; everything else reaches the application as a key event.
    std::vector<tui_event> apply_keys(const std::vector<tui_keyev> &keys)
    {
	std::vector<tui_event> out;
	std::string run;
	for ( size_t i = 0; i < keys.size(); ++i )
	{
	    const tui_keyev &k = keys[i];
	    if ( k.kind == tui_key::ch )
	    {
		run += k.ch;
		continue;
	    }
	    if ( !run.empty() )
	    {
		tui_event e;
		e.kind = tui_event_kind::text;
		e.text = run;
		out.push_back(e);
		run.clear();
	    }
	    const bool on_choice = _focus < _focusables.size()
		&& _focusables[_focus].k == focusable::kind::choice
		&& _focusables[_focus].option_count > 0;
	    if ( k.kind == tui_key::resize )
	    {
		tui_event e;
		e.kind = tui_event_kind::resize;
		out.push_back(e);
	    }
	    else if ( k.kind == tui_key::tab && _focusables.size() > 1 )
	    {
		_focus = (_focus + 1) % _focusables.size();
		tui_event e;
		e.kind = tui_event_kind::focus;
		out.push_back(e);
	    }
	    else if ( on_choice && (k.kind == tui_key::left
				 || k.kind == tui_key::up
				 || k.kind == tui_key::right
				 || k.kind == tui_key::down) )
	    {
		size_t n = _focusables[_focus].option_count;
		size_t sel = selection_of(_focus);
		if ( k.kind == tui_key::left || k.kind == tui_key::up )
		    sel = (sel + n - 1) % n;
		else
		    sel = (sel + 1) % n;
		_selection[_focus] = sel;
		tui_event e;
		e.kind = tui_event_kind::focus;
		out.push_back(e);
	    }
	    else if ( on_choice && k.kind == tui_key::enter )
	    {
		size_t sel = selection_of(_focus);
		tui_event e;
		e.kind = tui_event_kind::choose;
		e.option = sel;
		e.action = _focusables[_focus].option_actions[sel];
		out.push_back(e);
	    }
	    else
	    {
		tui_event e;
		e.kind = tui_event_kind::key;
		e.key = k.kind;
		e.ch = k.ch;
		out.push_back(e);
	    }
	}
	if ( !run.empty() )
	{
	    tui_event e;
	    e.kind = tui_event_kind::text;
	    e.text = run;
	    out.push_back(e);
	}
	return out;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_MODEL_H
